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

shared_info NO_COPY *cygwin_shared;
user_info NO_COPY *user_shared;
HANDLE NO_COPY cygwin_shared_h;
HANDLE NO_COPY cygwin_user_h;

static HANDLE
open_shared_dir (PUNICODE_STRING name, HANDLE root, NTSTATUS *status)
{
  OBJECT_ATTRIBUTES attr;
  HANDLE dir = NULL;

  InitializeObjectAttributes (&attr, name, OBJ_OPENIF, root,
			      everyone_sd (CYG_SHARED_DIR_ACCESS));
  *status = NtCreateDirectoryObject (&dir, CYG_SHARED_DIR_ACCESS, &attr);
  return NT_SUCCESS (*status) ? dir : NULL;
}

static bool
get_appcontainer_sid (cygsid &sid)
{
  static bool queried;
  static cygsid cached_sid (NO_SID);

  if (!queried)
    {
      HANDLE token = NULL;
      ULONG size = 0;
      NTSTATUS status;
      queried = true;
      if (NT_SUCCESS (NtOpenProcessToken (NtCurrentProcess (), TOKEN_QUERY,
					  &token)))
	{
	  status = NtQueryInformationToken (token, TokenAppContainerSid,
					    NULL, 0, &size);
	  if (status == STATUS_BUFFER_TOO_SMALL
	      || status == STATUS_BUFFER_OVERFLOW)
	    {
	      PTOKEN_APPCONTAINER_INFORMATION pac
		= (PTOKEN_APPCONTAINER_INFORMATION) alloca (size);
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

static bool
in_appcontainer ()
{
  cygsid sid (NO_SID);
  return get_appcontainer_sid (sid);
}

/* AppContainer processes cannot create objects in \\BaseNamedObjects.
   Use their private named-object directory instead. */
static HANDLE
get_appcontainer_parent_dir (NTSTATUS *status)
{
  HANDLE root = NULL, dir = NULL;
  cygsid sid (NO_SID);

  *status = STATUS_NOT_FOUND;
  if (!get_appcontainer_sid (sid))
    return NULL;

  PROCESS_SESSION_INFORMATION psi;
  *status = NtQueryInformationProcess (NtCurrentProcess (),
				       ProcessSessionInformation,
				       &psi, sizeof psi, NULL);
  if (!NT_SUCCESS (*status))
    return NULL;

  UNICODE_STRING usid;
  *status = RtlConvertSidToUnicodeString (&usid, sid, TRUE);
  if (!NT_SUCCESS (*status))
    return NULL;
  WCHAR path[MAX_PATH];
  __small_swprintf (path, L"\\Sessions\\%u\\AppContainerNamedObjects\\%S",
		    psi.SessionId, &usid);
  RtlFreeUnicodeString (&usid);

  UNICODE_STRING uname;
  RtlInitUnicodeString (&uname, path);
  root = open_shared_dir (&uname, NULL, status);
  if (root)
    {
      __small_swprintf (path, L"%s%s-%S", cygwin_version.shared_id,
			_cygwin_testing ? cygwin_version.dll_build_date : "",
			&cygheap->installation_key);
      RtlInitUnicodeString (&uname, path);
      dir = open_shared_dir (&uname, root, status);
      NtClose (root);
    }
  return dir;
}

/* This function returns a handle to the top-level directory in the global
   NT namespace used to implement global objects including shared memory. */

static HANDLE NO_COPY shared_parent_dir;

HANDLE
get_shared_parent_dir ()
{
  UNICODE_STRING uname;
  OBJECT_ATTRIBUTES attr;
  NTSTATUS status;

  if (!shared_parent_dir)
    {
      if (in_appcontainer ())
	{
	  shared_parent_dir = get_appcontainer_parent_dir (&status);
	  if (!shared_parent_dir)
	    api_fatal ("NtCreateDirectoryObject(AppContainer): %y", status);
	}
      else
	{
	  WCHAR bnoname[MAX_PATH];
	  __small_swprintf (bnoname, L"\\BaseNamedObjects\\%s%s-%S",
			    cygwin_version.shared_id,
			    _cygwin_testing ? cygwin_version.dll_build_date : "",
			    &cygheap->installation_key);
	  RtlInitUnicodeString (&uname, bnoname);
	  InitializeObjectAttributes (&attr, &uname, OBJ_OPENIF, NULL,
				      everyone_sd (CYG_SHARED_DIR_ACCESS));
	  status = NtCreateDirectoryObject (&shared_parent_dir,
					    CYG_SHARED_DIR_ACCESS, &attr);
	  if (!NT_SUCCESS (status))
	    api_fatal ("NtCreateDirectoryObject(%S): %y", &uname, status);
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
	  __small_swprintf (bnoname,
			    L"\\Sessions\\BNOLINKS\\%d\\%s%s-%S",
			    psi.SessionId, cygwin_version.shared_id,
			    _cygwin_testing ? cygwin_version.dll_build_date : "",
			    &cygheap->installation_key);
	  RtlInitUnicodeString (&uname, bnoname);
	  InitializeObjectAttributes (&attr, &uname, OBJ_OPENIF, NULL,
				      everyone_sd(CYG_SHARED_DIR_ACCESS));
	  status = NtCreateDirectoryObject (&session_parent_dir,
					    CYG_SHARED_DIR_ACCESS, &attr);
	  if (!NT_SUCCESS (status))
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

/* Create or open a named section in the AppContainer object directory. */
static HANDLE
open_shared_section (const WCHAR *name, DWORD size, DWORD access,
		     bool create, bool &created, PSECURITY_ATTRIBUTES psa)
{
  UNICODE_STRING uname;
  OBJECT_ATTRIBUTES attr;
  HANDLE h = NULL;
  NTSTATUS status;
  PSECURITY_DESCRIPTOR sd = psa ? (PSECURITY_DESCRIPTOR)
				  psa->lpSecurityDescriptor : NULL;

  /* Use explicit DACLs so sibling AppContainer processes can reopen
     sections. */
  if (create)
    {
      if (!sd)
	{
	  psa = sec_user_nih ((PSECURITY_ATTRIBUTES) alloca (1024),
			      cygheap->user.sid ());
	  sd = (PSECURITY_DESCRIPTOR) psa->lpSecurityDescriptor;
	}
      else
	{
	  BOOLEAN present = FALSE, defaulted = FALSE;
	  PACL dacl = NULL;
	  if (NT_SUCCESS (RtlGetDaclSecurityDescriptor (sd, &present, &dacl,
						       &defaulted))
	      && present && !dacl)
	    sd = everyone_sd (FILE_MAP_ALL_ACCESS);
	}
    }
  RtlInitUnicodeString (&uname, name);
  InitializeObjectAttributes (&attr, &uname, 0, get_shared_parent_dir (), sd);
  if (create)
    {
      LARGE_INTEGER max;
      max.QuadPart = size;
      status = NtCreateSection (&h, SECTION_ALL_ACCESS, &attr, &max,
				PAGE_READWRITE, SEC_COMMIT, NULL);
      if (status == STATUS_OBJECT_NAME_COLLISION
	  || status == STATUS_OBJECT_NAME_EXISTS)
	{
	  created = false;
	  status = NtOpenSection (&h, SECTION_ALL_ACCESS, &attr);
	}
    }
  else
    status = NtOpenSection (&h, access | SECTION_QUERY, &attr);
  if (!NT_SUCCESS (status))
    {
      SetLastError (RtlNtStatusToDosError (status));
      return NULL;
    }
  return h;
}

HANDLE
open_shared_mutex (const WCHAR *name, int n)
{
  WCHAR buf[MAX_PATH];
  shared_name (buf, name, n);
  if (!in_appcontainer ())
    return CreateMutexW (&sec_all_nih, FALSE, buf);

  UNICODE_STRING uname;
  OBJECT_ATTRIBUTES attr;
  HANDLE h = NULL;
  RtlInitUnicodeString (&uname, buf);
  InitializeObjectAttributes (&attr, &uname, OBJ_OPENIF,
			      get_shared_parent_dir (),
			      everyone_sd (CYG_MUTANT_ACCESS));
  NTSTATUS status = NtCreateMutant (&h, CYG_MUTANT_ACCESS, &attr, FALSE);
  if (!NT_SUCCESS (status))
    SetLastError (RtlNtStatusToDosError (status));
  return h;
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
  const bool ac = name && in_appcontainer ();

  if (ac)
    shared_h = NULL;
  created = false;
  if (!shared_h)
    {
      if (name)
	mapname = shared_name (map_buf, name, n);
      if (ac)
	{
	  created = m != SH_JUSTOPEN;
	  shared_h = open_shared_section (mapname, size, access,
					  m != SH_JUSTOPEN, created, psa);
	}
      else if (m == SH_JUSTOPEN)
	shared_h = OpenFileMappingW (access, FALSE, mapname);
      else
	{
	  created = true;
	  shared_h = CreateFileMappingW (INVALID_HANDLE_VALUE, psa,
					PAGE_READWRITE, 0, size, mapname);
	  if (GetLastError () == ERROR_ALREADY_EXISTS)
	    created = false;
	}
      if (shared_h)
	/* ok! */;
      else if (m != SH_JUSTOPEN)
	api_fatal ("CreateFileMapping %W, %E.  Terminating.", mapname);
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
  while (!shared);

  if (!shared)
    api_fatal ("MapViewOfFileEx '%W'(%p, size %u, m %d, created %d), %E.  "
	       "Terminating.", mapname, shared_h, size, m, created);

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

  if (!cygwin_user_h || in_appcontainer ())
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
  get_shared_parent_dir ();
  shared_info::create ();	/* Initialize global shared memory */
  cygheap->user_heap.init ();	/* Initialize user heap */
  user_info::create (false);	/* Initialize per-user shared memory */
  /* Initialize tty list session stuff.  Doesn't really belong here but
     this needs to be initialized before any tty or console manipulation
     happens and it is a common location.  */
  tty_list::init_session ();
}
