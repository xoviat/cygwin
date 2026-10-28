/* shared.cc: shared data area support.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include "miscfuncs.h"
#include <unistd.h>
#include "cygerrno.h"
#include "pinfo.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "heap.h"
#include "shared_info_magic.h"
#include "registry.h"
#include "cygwin_version.h"
#include "memory_layout.h"
#include "spinlock.h"
#include <alloca.h>
#include <wchar.h>
#include "security.h"

shared_info NO_COPY *cygwin_shared;
user_info NO_COPY *user_shared;
HANDLE NO_COPY cygwin_shared_h;
HANDLE NO_COPY cygwin_user_h;

/* AppContainer / restricted-sandbox support.

   AppContainer processes (and sandbox backends that hand out AppContainer
   tokens, such as MXC's ProcessContainer) cannot create objects in the
   global \BaseNamedObjects namespace: Win32 named-object APIs are
   redirected in user-mode (kernelbase!BaseGetNamedObjectDirectory) to
   \Sessions\<session>\AppContainerNamedObjects\<package SID>, but absolute
   NT paths passed to NtCreateDirectoryObject are not rewritten and fail
   the DIRECTORY_CREATE_SUBDIRECTORY access check with STATUS_ACCESS_DENIED
   (see microsoft/mxc#1061).  When the global namespace is not writable,
   fall back to the container's own named-object namespace, keeping the
   per-installation key as a subdirectory so multiple MSYS installations
   sharing one container do not collide. */

/* Cached per-installation key leaf name, e.g. "msys-2.0S5-<key>". */
static WCHAR NO_COPY shared_key_name[128];

static void
build_shared_key_name ()
{
  if (!shared_key_name[0])
    __small_swprintf (shared_key_name, L"%s%s-%S",
		      cygwin_version.shared_id,
		      _cygwin_testing ? cygwin_version.dll_build_date : "",
		      &cygheap->installation_key);
}

/* Query the current session id. */
static bool
get_session_id (ULONG *id)
{
  PROCESS_SESSION_INFORMATION psi;
  NTSTATUS status = NtQueryInformationProcess (NtCurrentProcess (),
					      ProcessSessionInformation,
					      &psi, sizeof psi, NULL);
  if (!NT_SUCCESS (status))
    return false;
  *id = psi.SessionId;
  return true;
}

/* Query the AppContainer package SID of the current process token via
   ntdll.  ntdll is used deliberately: some sandboxes filter
   kernelbase's QueryInformationToken for selected token classes while
   the raw NT query still answers.  Returns false (and NO_SID) when the
   process is not running under an AppContainer-like token. */
static bool
get_appcontainer_sid (cygsid &sid)
{
  static bool queried = false;
  static cygsid cached_sid (NO_SID);

  if (!queried)
    {
      HANDLE token = NULL;
      ULONG size = 0;
      queried = true;
      if (NT_SUCCESS (NtOpenProcessToken (NtCurrentProcess (),
					  TOKEN_QUERY, &token)))
	{
	  NTSTATUS status = NtQueryInformationToken (token,
						    TokenAppContainerSid,
						    NULL, 0, &size);
	  if (status == STATUS_BUFFER_TOO_SMALL
	      || status == STATUS_BUFFER_OVERFLOW)
	    {
	      PTOKEN_APPCONTAINER_INFORMATION pac =
		  (PTOKEN_APPCONTAINER_INFORMATION) alloca (size);
	      status = NtQueryInformationToken (token, TokenAppContainerSid,
						pac, size, &size);
	      if (NT_SUCCESS (status) && pac->TokenAppContainer)
		cached_sid = pac->TokenAppContainer;
	    }
	  NtClose (token);
	}
    }
  sid = cached_sid;
  return PSID (sid) != NO_SID;
}

/* Open (or create) a directory object NAME relative to ROOT (or with an
   absolute path when ROOT is NULL).  Returns the handle, or NULL with
   *err set to the failing NTSTATUS. */
static HANDLE
open_shared_dir (PUNICODE_STRING name, HANDLE root, NTSTATUS *err,
		 bool retry = false)
{
  OBJECT_ATTRIBUTES attr;
  HANDLE dir = NULL;

  InitializeObjectAttributes (&attr, name, OBJ_OPENIF, root,
			      everyone_sd (CYG_SHARED_DIR_ACCESS));
  for (int tries = 0;; tries++)
    {
      *err = NtCreateDirectoryObject (&dir, CYG_SHARED_DIR_ACCESS, &attr);
      if (NT_SUCCESS (*err) || !retry || tries >= 100)
	break;
      /* Sandboxed environments (e.g. the Windows ProcessContainer) can
	 make the named object namespace of a new process available
	 slightly after it starts running.  A create at that instant
	 fails transiently even though the same call succeeds
	 milliseconds later.  Retry briefly before giving up. */
      if (*err != STATUS_ACCESS_DENIED
	  && *err != STATUS_OBJECT_NAME_NOT_FOUND
	  && *err != STATUS_OBJECT_PATH_NOT_FOUND
	  && *err != STATUS_OBJECT_NAME_COLLISION)
	break;
      Sleep (20);
    }
  return NT_SUCCESS (*err) ? dir : NULL;
}

/* The per-installation directory inside the AppContainer named-object
   namespace
   (\Sessions\<session>\AppContainerNamedObjects\<package SID>\<key>).
   The container root directory is created by the kernel at AppContainer
   process creation (verified writable from inside the container, with
   CYG_SHARED_DIR_ACCESS).  Returns NULL and sets *err when the token has
   no AppContainer SID or the directory cannot be opened/created. */
static HANDLE
get_appcontainer_parent_dir (NTSTATUS *err)
{
  static HANDLE NO_COPY ac_parent_dir;
  static bool NO_COPY ac_tried = false;
  cygsid sid (NO_SID);
  ULONG session = 0;
  WCHAR buf[MAX_PATH];
  UNICODE_STRING uname;
  HANDLE root = NULL;

  *err = STATUS_UNSUCCESSFUL;
  if (ac_tried)
    return ac_parent_dir;
  ac_tried = true;

  if (!get_appcontainer_sid (sid))
    {
      *err = STATUS_NOT_FOUND;
      return NULL;
    }
  if (!get_session_id (&session))
    return NULL;
  build_shared_key_name ();

  {
    UNICODE_STRING ac_sid;
    NTSTATUS st = RtlConvertSidToUnicodeString (&ac_sid, sid, TRUE);
    if (!NT_SUCCESS (st))
      {
	*err = st;
	return NULL;
      }
    __small_swprintf (buf,
		      L"\\Sessions\\%d\\AppContainerNamedObjects\\%S",
		      session, &ac_sid);
    RtlFreeUnicodeString (&ac_sid);
  }

  RtlInitUnicodeString (&uname, buf);
  root = open_shared_dir (&uname, NULL, err, true);
  if (!root)
    return NULL;

  {
    UNICODE_STRING key;
    RtlInitUnicodeString (&key, shared_key_name);
    ac_parent_dir = open_shared_dir (&key, root, err, true);
  }
  NtClose (root);
  return ac_parent_dir;
}

/* This function returns a handle to the top-level directory in the global
   NT namespace used to implement global objects including shared memory.

   When the process runs under a sandbox that denies write access to the
   global namespace (AppContainer-based sandboxes such as MXC's
   ProcessContainer), fall back to the container's named-object namespace
   and then to the session namespaces before giving up. */

static HANDLE NO_COPY shared_parent_dir;

HANDLE
get_shared_parent_dir ()
{
  UNICODE_STRING uname;
  OBJECT_ATTRIBUTES attr;
  NTSTATUS status, ac_status = STATUS_UNSUCCESSFUL;
  NTSTATUS bnol_status = STATUS_UNSUCCESSFUL;
  NTSTATUS sbno_status = STATUS_UNSUCCESSFUL;

  if (!shared_parent_dir)
    {
      WCHAR bnoname[MAX_PATH];
      build_shared_key_name ();
      __small_swprintf (bnoname, L"\\BaseNamedObjects\\%s", shared_key_name);
      RtlInitUnicodeString (&uname, bnoname);
      InitializeObjectAttributes (&attr, &uname, OBJ_OPENIF, NULL,
				  everyone_sd (CYG_SHARED_DIR_ACCESS));
      status = NtCreateDirectoryObject (&shared_parent_dir,
					CYG_SHARED_DIR_ACCESS, &attr);
      if (!NT_SUCCESS (status))
	{
	  /* The global namespace is not writable (sandboxed process).
	     Use the AppContainer named-object namespace when the token
	     has a package SID... */
	  shared_parent_dir = get_appcontainer_parent_dir (&ac_status);
	  if (!shared_parent_dir)
	    {
	      /* ... otherwise the per-session namespaces. */
	      ULONG session = 0;
	      if (get_session_id (&session) && session)
		{
		  __small_swprintf (bnoname,
				    L"\\Sessions\\BNOLINKS\\%d\\%s",
				    session, shared_key_name);
		  RtlInitUnicodeString (&uname, bnoname);
		  shared_parent_dir = open_shared_dir (&uname, NULL,
						      &bnol_status);
		  if (!shared_parent_dir)
		    {
		      __small_swprintf (bnoname,
					L"\\Sessions\\%d\\"
					L"BaseNamedObjects\\%s",
					session, shared_key_name);
		      RtlInitUnicodeString (&uname, bnoname);
		      shared_parent_dir = open_shared_dir (&uname, NULL,
							  &sbno_status);
		    }
		}
	      if (!shared_parent_dir)
		api_fatal ("NtCreateDirectoryObject"
			   "(\\BaseNamedObjects\\%s): %y; "
			   "AppContainer ns: %y; BNOLINKS: %y; "
			   "session BNO: %y",
			   shared_key_name, status, ac_status,
			   bnol_status, sbno_status);
	    }
	}
    }
  return shared_parent_dir;
}

static HANDLE NO_COPY session_parent_dir;

HANDLE
get_session_parent_dir ()
{
  UNICODE_STRING uname;
  OBJECT_ATTRIBUTES attr;
  NTSTATUS status;

  if (!session_parent_dir)
    {
      PROCESS_SESSION_INFORMATION psi;
      status = NtQueryInformationProcess (NtCurrentProcess (),
					  ProcessSessionInformation,
					  &psi, sizeof psi, NULL);
      if (!NT_SUCCESS (status) || psi.SessionId == 0)
	session_parent_dir = get_shared_parent_dir ();
      else
	{
	  WCHAR bnoname[MAX_PATH];
	  build_shared_key_name ();
	  __small_swprintf (bnoname,
			    L"\\Sessions\\BNOLINKS\\%d\\%s",
			    psi.SessionId, shared_key_name);
	  RtlInitUnicodeString (&uname, bnoname);
	  InitializeObjectAttributes (&attr, &uname, OBJ_OPENIF, NULL,
				      everyone_sd(CYG_SHARED_DIR_ACCESS));
	  status = NtCreateDirectoryObject (&session_parent_dir,
					    CYG_SHARED_DIR_ACCESS, &attr);
	  if (!NT_SUCCESS (status))
	    /* Sandboxed process: the session namespace is not writable
	       either.  Share the fallback chain used for the global
	       parent dir (verified correct under MXC's ProcessContainer,
	       where both \\Sessions\\BNOLINKS\\<n> and
	       \\Sessions\\<n>\\BaseNamedObjects are denied but the
	       AppContainer namespace is writable). */
	    session_parent_dir = get_shared_parent_dir ();
	}
    }
  return session_parent_dir;
}

char *
shared_name (char *ret_buf, const char *str, int num)
{
  __small_sprintf (ret_buf, "%s.%d", str, num);
  return ret_buf;
}

WCHAR *
shared_name (WCHAR *ret_buf, const WCHAR *str, int num)
{
  __small_swprintf (ret_buf, L"%W.%d", str, num);
  return ret_buf;
}


/* Absolute NT path of NAME.N in the AppContainer namespace ROOT:
   \Sessions\<session>\AppContainerNamedObjects\<package SID>\NAME.N
   Win32 bare names are redirected elsewhere (per-installation-key
   subdirectories once cygwin created those); under mxc's
   ProcessContainer the sections then end up unopenable for sibling
   processes.  Objects at the namespace root open fine everywhere
   (verified in-container), and every cygwin process derives this exact
   path from its own package SID + session id, so all processes agree.
   Returns false when the token has no AppContainer SID (plain
   Windows): callers then keep the stock Win32 behavior unchanged. */
static bool
ac_absolute_name (const WCHAR *name, int n, WCHAR *buf, size_t bufsz)
{
  cygsid pac_sid (NO_SID);
  ULONG ac_session = 0;
  UNICODE_STRING usid;
  WCHAR leaf[MAX_PATH];

  if (!name || !get_appcontainer_sid (pac_sid)
      || !get_session_id (&ac_session))
    return false;
  if (!NT_SUCCESS (RtlConvertSidToUnicodeString (&usid, pac_sid, TRUE)))
    return false;
  __small_swprintf (buf, L"\\Sessions\\%d\\AppContainerNamedObjects\\%S",
		    (int) ac_session, &usid);
  RtlFreeUnicodeString (&usid);
  shared_name (leaf, name, n);
  if (wcslen (buf) + 1 + wcslen (leaf) + 1 > bufsz)
    return false;
  wcscat (buf, L"\\");
  wcscat (buf, leaf);
  return true;
}

static HANDLE
nt_open_section_name (const WCHAR *path, DWORD access)
{
  UNICODE_STRING uname;
  OBJECT_ATTRIBUTES attr;
  HANDLE h = NULL;

  RtlInitUnicodeString (&uname, path);
  InitializeObjectAttributes (&attr, &uname, 0, NULL, NULL);
  if (!NT_SUCCESS (NtOpenSection (&h, access | SECTION_QUERY, &attr)))
    return NULL;
  return h;
}

static NTSTATUS
nt_create_section_name (const WCHAR *path, PSECURITY_ATTRIBUTES psa,
			DWORD size, HANDLE *out, bool *existed)
{
  UNICODE_STRING uname;
  OBJECT_ATTRIBUTES attr;
  LARGE_INTEGER ms;
  PSECURITY_DESCRIPTOR sd =
    psa ? (PSECURITY_DESCRIPTOR) psa->lpSecurityDescriptor : NULL;
  NTSTATUS st;

  *existed = false;
  *out = NULL;
  RtlInitUnicodeString (&uname, path);
  InitializeObjectAttributes (&attr, &uname, 0, NULL, sd);
  ms.QuadPart = size;
  st = NtCreateSection (out, SECTION_ALL_ACCESS, &attr, &ms,
			PAGE_READWRITE, SEC_COMMIT, NULL);
  if (st == STATUS_OBJECT_NAME_COLLISION || st == STATUS_OBJECT_NAME_EXISTS)
    *existed = true;
  else if (!NT_SUCCESS (st) && sd)
    {
      /* If some sandbox filters reject SD-bearing creates outright,
	 retry once without a descriptor. */
      *out = NULL;
      InitializeObjectAttributes (&attr, &uname, 0, NULL, NULL);
      st = NtCreateSection (out, SECTION_ALL_ACCESS, &attr, &ms,
			    PAGE_READWRITE, SEC_COMMIT, NULL);
      if (st == STATUS_OBJECT_NAME_COLLISION
	  || st == STATUS_OBJECT_NAME_EXISTS)
	*existed = true;
    }
  if (*existed)
    {
      /* The section is already there.  Some sandboxes report the
	 collision without handing back a handle, so open it. */
      *out = nt_open_section_name (path, FILE_MAP_ALL_ACCESS);
      if (!*out)
	return STATUS_ACCESS_DENIED;
      return STATUS_SUCCESS;
    }
  return st;
}

/* Create or open the installation-wide named mutex NAME.N.

   Bare Win32 object names are unreliable inside hardened sandboxes:
   for msys processes they are routed into per-installation keyed
   subdirectories, and creation/opening at that route was observed to
   fail with ERROR_ACCESS_DENIED for freshly created (forked) processes
   inside the mxc ProcessContainer while the identical call succeeds in
   the parent (tty_list::mutex).  Inside an AppContainer use the
   absolute namespace-root path instead -- the same addressing the
   section objects use (see ac_absolute_name), which every cygwin
   process derives identically, so all processes still share one mutex.
   Without a package SID the stock CreateMutex call is kept unchanged.
   Both paths retry the transient namespace-provisioning errors briefly
   (same race as the open_shared create path). */
HANDLE
open_shared_mutex (const WCHAR *name, int n)
{
  WCHAR leaf[MAX_PATH];
  WCHAR acname[MAX_PATH];
  PSECURITY_DESCRIPTOR sd = everyone_sd (CYG_MUTANT_ACCESS);
  bool ac = ac_absolute_name (name, n, acname,
			      sizeof acname / sizeof (WCHAR));

  shared_name (leaf, name, n);
  for (int tries = 0;; tries++)
    {
      HANDLE h = NULL;
      DWORD err = 0;

      if (ac)
	{
	  UNICODE_STRING uname;
	  OBJECT_ATTRIBUTES attr;
	  NTSTATUS st;

	  RtlInitUnicodeString (&uname, acname);
	  InitializeObjectAttributes (&attr, &uname, 0, NULL, sd);
	  st = NtCreateMutant (&h, CYG_MUTANT_ACCESS, &attr, FALSE);
	  if (!NT_SUCCESS (st))
	    {
	      /* Some sandboxes reject SD-bearing creates outright;
		 retry once without a descriptor. */
	      h = NULL;
	      InitializeObjectAttributes (&attr, &uname, 0, NULL, NULL);
	      st = NtCreateMutant (&h, CYG_MUTANT_ACCESS, &attr, FALSE);
	    }
	  if (!NT_SUCCESS (st))
	    {
	      /* Collision without a handle (see nt_create_section_name):
		 open the existing mutex explicitly. */
	      h = NULL;
	      InitializeObjectAttributes (&attr, &uname, 0, NULL, NULL);
	      st = NtOpenMutant (&h, CYG_MUTANT_ACCESS, &attr);
	    }
	  if (!NT_SUCCESS (st))
	    {
	      h = NULL;
	      err = RtlNtStatusToDosError (st);
	    }
	}
      else
	{
	  h = CreateMutexW (&sec_all_nih, FALSE, leaf);
	  if (!h)
	    err = GetLastError ();
	}
      if (h)
	return h;

      bool transient = (err == ERROR_ACCESS_DENIED
			|| err == ERROR_PATH_NOT_FOUND
			|| err == ERROR_FILE_NOT_FOUND
			|| err == ERROR_INVALID_NAME);
      if (!transient || tries >= 150)
	return NULL;
      Sleep (20);
    }
}

#define page_const ((ptrdiff_t) 65535)
#define pround(n) ((ptrdiff_t)(((n) + page_const) & ~page_const))

static NO_COPY uintptr_t next_address = SHARED_REGIONS_ADDRESS_LOW;


void *
open_shared (const WCHAR *name, int n, HANDLE& shared_h, DWORD size,
	     shared_locations m, PSECURITY_ATTRIBUTES psa, DWORD access)
{
  bool created_dummy;
  return open_shared (name, n, shared_h, size, m, created_dummy, psa, access);
}

void *
open_shared (const WCHAR *name, int n, HANDLE& shared_h, DWORD size,
	     shared_locations m, bool &created, PSECURITY_ATTRIBUTES psa,
	     DWORD access)
{
  WCHAR map_buf[MAX_PATH];
  WCHAR *mapname = NULL;
  void *shared;
  void *addr;

  /* Hardened sandboxes (e.g. the Windows ProcessContainer) silently
     rewrite DACL-less sections: the object gets created, but with a
     default DACL that then denies every sibling process (observed:
     forked cygwin children fail on shared.5 with ERROR_ACCESS_DENIED
     while fresh names in the same directory work fine, because the
     parent's NULL-DACL object was rewritten).  Substitute an explicit
     everyone-ACE descriptor, which survives rewriting -- the same
     shape we already use for our named-object directories.  Without a
     sandbox this only changes the SD from "no DACL" to "everybody
     allowed", which is access-equivalent.  (Keep this block at
     function scope: everyone_sd allocates its buffer on the stack.) */
  SECURITY_ATTRIBUTES explicit_sa;
  PSECURITY_DESCRIPTOR lsd =
    psa ? (PSECURITY_DESCRIPTOR) psa->lpSecurityDescriptor : NULL;
  BOOLEAN dacl_present = FALSE, dacl_defaulted = FALSE;
  PACL dacl = NULL;
  bool dacl_less = !lsd;
  if (lsd
      && NT_SUCCESS (RtlGetDaclSecurityDescriptor (lsd, &dacl_present,
						   &dacl, &dacl_defaulted)))
    dacl_less = dacl_present && !dacl;
  if (dacl_less)
    {
      explicit_sa.nLength = sizeof (SECURITY_ATTRIBUTES);
      explicit_sa.lpSecurityDescriptor = everyone_sd (FILE_MAP_ALL_ACCESS);
      explicit_sa.bInheritHandle = FALSE;
      psa = &explicit_sa;
    }


  created = false;
  /* Remember whether the caller handed us an already open (typically
     inherited) handle.  Such a borrowed value can be present in our
     handle table and still be the wrong object for mapping in this
     process: the classic fork emulation creates the child via
     CreateProcess and passes the parent's cygwin_user_h value along
     via child_proc_info, but hardened sandboxes (the Windows
     ProcessContainer) don't preserve the handle table entry for it
     (observed: MapViewOfFileEx fails with ERROR_INVALID_HANDLE
     although GetHandleInformation reports the value as a valid
     handle).  In that case drop the borrowed handle and redo the
     create/open below by name. */
  bool preset = shared_h != NULL;
  /* Prefer the named path whenever a name is available: in sandboxed fork
     children (mxc ProcessContainer) inherited handle values can be
     present-but-reference the wrong object, and MapViewOfFileEx on such a
     value can silently succeed on the wrong object.  The borrowed handle is
     kept only as a fallback. */
  HANDLE borrowed = shared_h;
  if (preset && name != NULL)
    shared_h = NULL;
retry_borrowed_handle:
  if (!shared_h)
    {
      if (name)
	mapname = shared_name (map_buf, name, n);
      /* AppContainer: use the absolute namespace-root path (see
	 ac_absolute_name).  Without an AppContainer SID, ntname stays
	 NULL and stock Win32 behavior is preserved. */
      WCHAR ac_map_buf[MAX_PATH];
      NTSTATUS nt_last_st = STATUS_SUCCESS;
      WCHAR *ntname = NULL;
      if (name && ac_absolute_name (name, n, ac_map_buf,
				    sizeof ac_map_buf / sizeof (WCHAR)))
	ntname = ac_map_buf;
      if (m == SH_JUSTOPEN)
	shared_h = ntname ? nt_open_section_name (ntname, access)
			  : OpenFileMappingW (access, FALSE, mapname);
      else
	{
	  created = true;
	  /* See the DACL-less substitution at function top. */
	  /* Sandboxed environments (e.g. the Windows ProcessContainer) can
	     make the named object namespace of a newly created process
	     available slightly after it starts running.  A create at that
	     instant fails with ACCESS_DENIED/PATH_NOT_FOUND even though
	     the very same call succeeds milliseconds later (observed for
	     nested processes in the mxc ProcessContainer, where every
	     forked cygwin child would otherwise die here).  Retry
	     transient failures briefly before treating them as fatal. */
	  for (int tries = 0;; tries++)
	    {
	      NTSTATUS st = STATUS_SUCCESS;
	      if (ntname)
		{
		  bool existed = false;
		  st = nt_create_section_name (ntname, psa, size,
					       &shared_h, &existed);
		  nt_last_st = st;
		  if (NT_SUCCESS (st))
		    {
		      if (existed)
			created = false;
		      break;
		    }
		}
	      else
		{
		  shared_h = CreateFileMappingW (INVALID_HANDLE_VALUE, psa,
						PAGE_READWRITE, 0, size,
						mapname);
		  if (shared_h)
		    break;
		}
	      bool transient;
	      if (ntname)
		transient = (st == STATUS_ACCESS_DENIED
			     || st == STATUS_OBJECT_NAME_NOT_FOUND
			     || st == STATUS_OBJECT_PATH_NOT_FOUND
			     || st == STATUS_OBJECT_NAME_INVALID);
	      else
		{
		  DWORD err = GetLastError ();
		  transient = (err == ERROR_ACCESS_DENIED
			       || err == ERROR_PATH_NOT_FOUND
			       || err == ERROR_FILE_NOT_FOUND
			       || err == ERROR_INVALID_NAME);
		}
	      if (!transient || tries >= 150)
		{
		  if (ntname && !NT_SUCCESS (nt_last_st))
		    SetLastError (RtlNtStatusToDosError (nt_last_st));
		  break;
		}
	      Sleep (20);
	    }
	  if (!ntname && shared_h && GetLastError () == ERROR_ALREADY_EXISTS)
	    created = false;
	}
      if (shared_h)
	/* ok! */;
      else if (preset && borrowed)
	{
	  /* Named path failed; fall back to the borrowed handle value. */
	  shared_h = borrowed;
	  preset = false;
	  created = false;
	  goto retry_borrowed_handle;
	}
      else if (m != SH_JUSTOPEN)
	api_fatal ("CreateFileMapping %W, %E.  Terminating.",
		   ntname ? ntname : mapname);
      else
	return NULL;
    }

  /* Locate shared regions in the area between SHARED_REGIONS_ADDRESS_LOW
     and SHARED_REGIONS_ADDRESS_HIGH, retrying until we have a slot.
     Don't use MapViewOfFile3 (STATUS_DLL_INIT_FAILED during fork). */
  bool loop = false;

  do
    {
      addr = (void *) next_address;
      shared = MapViewOfFileEx (shared_h, access, 0, 0, 0, addr);
      next_address += wincap.allocation_granularity ();
      if (next_address >= SHARED_REGIONS_ADDRESS_HIGH)
	{
	  if (!shared && loop)
	    break;
	  next_address = SHARED_REGIONS_ADDRESS_LOW;
	  loop = true;
	}
    }
  /* ERROR_INVALID_HANDLE means the handle value itself is unusable;
     retrying other addresses cannot help. */
  while (shared == NULL && GetLastError () != ERROR_INVALID_HANDLE);

  if (!shared)
    {
      if (preset && borrowed)
        {
          /* Named mapping failed; try the borrowed handle value once. */
          preset = false;
          shared_h = borrowed;
          created = false;
          goto retry_borrowed_handle;
        }
    }

  if (!shared)
    {
    api_fatal ("MapViewOfFileEx '%W'(%p, size %u, m %d, created %d), %E.  "
	       "Terminating.", mapname, shared_h, size, m, created);
    }

  debug_printf ("name %W, shared %p (wanted %p), h %p, m %d, created %d",
		mapname, shared, addr, shared_h, m, created);

  return shared;
}

/* Second half of user shared initialization: Initialize content. */
void
user_info::initialize ()
{
  /* Wait for initialization of the Cygwin per-user shared, if necessary */
  spinlock sversion (version, CURR_USER_MAGIC);
  if (!sversion)
    {
      cb = sizeof (*user_shared);
      /* Initialize mount table from system fstab prior to calling
         internal_getpwsid.  This allows to convert pw_dir and pw_shell
	 paths given in DOS notation to valid POSIX paths.  */
      mountinfo.init (false);
      cygpsid sid (cygheap->user.sid ());
      struct passwd *pw = internal_getpwsid (sid);
      /* Correct the user name with what's defined in /etc/passwd before
	 loading the user fstab file. */
      if (pw)
	cygheap->user.set_name (pw->pw_name);
      /* After fetching the user infos, add mount entries from user's fstab. */
      mountinfo.init (true);
    }
  else if (sversion != CURR_USER_MAGIC)
    sversion.multiple_cygwin_problem ("user shared memory version", version,
				      sversion);
  else if (user_shared->cb != sizeof (*user_shared))
    sversion.multiple_cygwin_problem ("user shared memory size", cb,
				      sizeof (*user_shared));
}

/* First half of user shared initialization: Create shared mem region. */
void
user_info::create (bool reinit)
{
  WCHAR name[UNLEN + 1] = L""; /* Large enough for SID */

  if (reinit)
    {
      if (!UnmapViewOfFile (user_shared))
	debug_printf("UnmapViewOfFile %E");
      if (!ForceCloseHandle (cygwin_user_h))
	debug_printf("CloseHandle %E");
      cygwin_user_h = NULL;
    }

  /* Always derive the name, even when an apparently usable handle was
     inherited: if the borrowed handle turns out to be unmappable (see
     the retry in open_shared), the section has to be re-opened by
     name, and the caller must have supplied it. */
  cygheap->user.get_windows_id (name);

  user_shared = (user_info *) open_shared (name, USER_VERSION,
					   cygwin_user_h, sizeof (user_info),
					   SH_USER_SHARED, &sec_none);
  debug_printf ("opening user shared for '%W' at %p", name, user_shared);
  ProtectHandleINH (cygwin_user_h);
  debug_printf ("user shared version %x", user_shared->version);
  if (reinit)
    user_shared->initialize ();
  cygheap->shared_regions.user_shared_addr = user_shared;
}

void
shared_destroy ()
{
  ForceCloseHandle (cygwin_shared_h);
  UnmapViewOfFile (cygwin_shared);
  ForceCloseHandle (cygwin_user_h);
  UnmapViewOfFile (user_shared);
}

/* Initialize obcaseinsensitive.*/
void
shared_info::init_obcaseinsensitive ()
{
  /* Instead of reading the obcaseinsensitive registry value, test the
     actual state of case sensitivity handling in the kernel. */
  UNICODE_STRING sysroot;
  OBJECT_ATTRIBUTES attr;
  HANDLE h;

  RtlInitUnicodeString (&sysroot, L"\\SYSTEMROOT");
  InitializeObjectAttributes (&attr, &sysroot, 0, NULL, NULL);
  /* NtOpenSymbolicLinkObject returns STATUS_ACCESS_DENIED when called
     with a 0 access mask.  However, if the kernel is case sensitive,
     it returns STATUS_OBJECT_NAME_NOT_FOUND because we used the incorrect
     case for the filename (It's actually "\\SystemRoot"). */
  obcaseinsensitive = NtOpenSymbolicLinkObject (&h, 0, &attr)
		      != STATUS_OBJECT_NAME_NOT_FOUND;
}

void inline
shared_info::create ()
{
  cygwin_shared = (shared_info *) open_shared (L"shared",
					       CYGWIN_VERSION_SHARED_DATA,
					       cygwin_shared_h,
					       sizeof (*cygwin_shared),
					       SH_CYGWIN_SHARED,
					       &sec_all_nih);
  cygwin_shared->initialize ();
  cygheap->shared_regions.cygwin_shared_addr = cygwin_shared;
}

void
shared_info::initialize ()
{
  spinlock sversion (version, CURR_SHARED_MAGIC);
  if (!sversion)
    {
      LUID luid;

      cb = sizeof (*this);
      get_session_parent_dir ();	/* Create session dir if first process. */
      init_obcaseinsensitive ();	/* Initialize obcaseinsensitive */
      tty.init ();			/* Initialize tty table  */
      mt.initialize ();			/* Initialize shared tape information */
      loadavg.initialize ();		/* Initialize loadavg information */
      NtAllocateLocallyUniqueId (&luid);/* Initialize pid_src to a low    */
      InterlockedExchange (&pid_src,	/* random value to make start pid */
		   luid.LowPart % 2048);/* less predictably               */
      forkable_hardlink_support = 0;    /* 0: Unknown, 1: Yes, -1: No */
      /* Defer debug output printing the installation root and installation key
	 up to this point.  Debug output except for system_printf requires
	 the global shared memory to exist. */
      debug_printf ("Installation root: <%W> key: <%S>",
		    cygheap->installation_root, &cygheap->installation_key);
    }
  else if (sversion != CURR_SHARED_MAGIC)
    sversion.multiple_cygwin_problem ("system shared memory version",
				      sversion, CURR_SHARED_MAGIC);
  else if (cb != sizeof (*this))
    system_printf ("size of shared memory region changed from %lu to %u",
		   sizeof (*this), cb);
}

void
memory_init ()
{
  shared_info::create ();	/* Initialize global shared memory */
  cygheap->user_heap.init ();	/* Initialize user heap */
  user_info::create (false);	/* Initialize per-user shared memory */
  /* Initialize tty list session stuff.  Doesn't really belong here but
     this needs to be initialized before any tty or console manipulation
     happens and it is a common location.  */
  tty_list::init_session ();
}
