# Wedged children: `cygheap_protect` SRWLOCK deadlock (2026-11-11)

> **LOCK WEDGE RESOLVED — SEH unwind now releases the abandoned acquisition.
> The separate underlying EFAULT/segfault issue remains open.**
> Companion to [FORK_FAILURES.md](FORK_FAILURES.md) (same failure domain: children of
> `fork`/`spawn`/`exec` dying or hanging in early runtime init). This file documents a
> *different* signature observed on 2026-11-11: children that stay alive but block
> **forever** on the cygheap allocator lock.

## Resolution (2026-11-12): release the owner during SEH unwind

The updated ProcessContainer repro was deterministic: a 200-concurrent-`diff`
burst produced 200/200 wedged children with the stock GitHub `v0.4.0` DLL.
An instrumented build reproduced the same wedges and captured the actual owner.

### Root cause

1. `_cmalloc()` acquires `cygheap_protect` and calls `_csbrk()`.
2. A subsequent write to the entry returned by `_csbrk()` raises
   `STATUS_ACCESS_VIOLATION` because that page is still `MEM_RESERVE`.
3. `exception::myfault` unwinds directly to the outer `__try` recovery target.
   This bypasses `init_cygheap::unlock()`; it is not a signal-handler or a
   terminated-helper-thread problem.
4. The next allocation on the **same TID** enters the non-recursive SRWLOCK and
   permanently waits on itself.  The captured sequence was:

   ```text
   lock-enter(cstrdup) -> lock-acquired -> csbrk-exit
   -> SEH unwind(0xc0000005, fault at returned entry)
   -> cmalloc-enter(cmalloc_abort) -> lock-enter(same TID, lock already 1)
   ```

   LLDB independently confirmed the final `fstatat()` → path conversion →
   `_cmalloc()` wait stack.  No signal setup or handler events occurred.

### Fix

`init_cygheap` now records the owning Windows TID on acquisition and clears it
on release.  `exception::myfault` and the alternate-stack unwind path call
`init_cygheap::unlock_on_unwind()` before unwinding, releasing only the lock
owned by the current thread.  The owner is `NO_COPY`, so children start with no
inherited owner.  The underlying exception and its `EFAULT` are deliberately
not hidden.

`tools/final-verify.sh` gained a 20-child regression burst.  It requires a
completion marker from every child but intentionally ignores child exit status,
because the separate memory fault must not be allowed to recreate a permanent
hang.

### Verified result and remaining issue

With the fix, all 200/200 children in the stock-wedge burst completed instead
of hanging.  They reported the underlying `diff: ... Bad address`, allowing the
caller/model to recover immediately.  The complete verification gate also
passed `RESULT: 15 passed, 0 failed`, including the existing fork churn and the
new no-wedge regression.  This is a **partial but direct fix for the permanent
lock wedge**, not a claim that the memory fault is fixed.

The memory fault remains under investigation.  A 1-MiB initial-commit
experiment produced 197/200 successful children (three segfaults), but is not
included as a fix.  A tiny native reserve/commit/write test succeeds both inside
and outside ProcessContainer, so this is not a generic Windows
`VirtualAlloc(MEM_COMMIT)` failure.

## Earlier investigation blocker (superseded by the ProcessContainer repro)

The stock-runtime trace and the later instrumented ProcessContainer trace below
are retained as the investigation history.  The request for a better repro has
been answered; see the resolution above.

New facts from this follow-up:

* The installed `C:\Program Files\Git\usr\bin\msys-2.0.dll` is **byte-identical**
  to the GitHub `v0.4.0` release asset: size `25,057,808`, SHA-256
  `f8f7af36410ccc7c724cf48570c489de60e51ec687488e25e85d79285a0b57df`.
  The release workflow built tag `v0.4.0` at commit `7fd6d6ef1`; the current
  branch's later commit only touches `uinfo.cc`, so the relevant source is known.
* The current **unconfined** Git Bash using that released DLL did reproduce the
  wedge once in a small sequential `diff`/`cmp` loop. Fresh `diff` PID 15336 had
  lock word `0x00000007ffffb203` and the same `_cmalloc` stack. The ProcessContainer
  is therefore not a necessary trigger. The process tree was killed after capture.
* After that one capture, the same released DLL completed 2,400 additional
  sequential/concurrent `diff`/`cmp` children without another wedge.
* A private build with an allocation-free, NO_COPY, in-memory ring around
  `lock()`/`unlock()` and thread attach/detach completed 3,640
  sequential/concurrent `diff`/`cmp` children without a wedge. Thus no owner
  trace was captured. This is a timing/layout-sensitive Heisenbug, not a clean
  bill of health: the ring also moved `cygheap_protect` from RVA `0x289d08` to
  RVA `0x28d520`, so both timing and neighboring-data corruption remain possible.
* H2 below should not be treated as established. `cygheap_protect` is in
  `.data_cygwin_nocopy`, explicitly after `__data_end__` in `cygwin.sc.in`, so the
  normal fork DLL-data copy excludes it. A copied parent-held lock requires proof
  of a copy-range bug or memory corruption, not merely the observed lock word.

No candidate source fix came from this earlier unconfined testing; the private instrumentation was removed before the final ProcessContainer diagnosis.

## Reproduction tips from the original session (2026-11-12 — answers the blocker above)

The environment of the original session differs from a plain console in three ways worth
re-creating, and the wedge can be confirmed **after the fact without an instrumented
build** (see "Owner evidence without instrumentation" — that section also answers the
"captured under instrumentation" blocker directly).

### 1. Reproduce through pig with a piped prompt (the faithful vehicle)

**What "pig" is** (for readers coming from the cygwin side): the coding-agent harness the
original session ran under — PiG, a Go agent tool at
`C:\Users\<user>\AppData\Local\Programs\pig\pig.exe`. Its model-driven **bash tool**
spawns `C:\Program Files\Git\bin\bash.exe` — i.e. the patched runtime — with **pipes** on
stdin/stdout/stderr, optionally inside the pig sandbox (the original burst occurred with
the sandbox enabled). Every wedged child in the original session was forked from exactly
such a tool call. So the suggestion is: run pig non-interactively with a **piped prompt**
for the default model, instructing it to run the command inside the sandbox — that
recreates the full vehicle in one line:

```bash
# pig switches to print mode when stdin/stdout is a pipe and builds the prompt from
# piped stdin (see pig's cli-integration docs). The prompt asks the model to run the
# loop with its bash tool:
printf '%s\n' 'Use your bash tool to run exactly these commands inside the sandbox, then report what you see:
for i in $(seq 1 200); do ( diff wedge-a.tmp wedge-b.tmp; echo $? > m.$i ) & done
sleep 20
echo survived: $(ls m.* 2>/dev/null | wc -l) of 200' | pig

# equivalents: pig -p "prompt"    pig --no-stdin -p "prompt"    pig @repro-prompt.txt
```

If the loop alone is clean, add the seeding from §2 to the same prompt (a fork-churn
pipeline first, then the loop).

Why the vehicle matters technically: the tool's bash has **pipe** fhandlers on all three
stdio fds, and the patch's most delicate area is `fhandler/pipe.cc` (absolute NT pipe
names, CreatePipe-anonymous `\Device\NamedPipe\pipe-%u-` naming,
create-without-OBJ_INHERIT + post-hoc `SetHandleInformation`, `need_fork_fixup(true)`
cross-process handle re-dup, `raw_write` mutex-recreate recovery). All of it runs against
the **parent's** fhandler table at every fork — including when the child's own fds are
redirected away (observed: `diff a b > out 2> err 0< /dev/null` still wedged). A plain
console bash replaces those pipe fhandlers with console fhandlers, so the suspect path
may never run — this likely explains clean loops in a console. Pipe-fed shells
(`cat repro.sh | bash`) approximate the condition, but the pig invocation above is the
exact original environment.

### 2. Seed the system state — the trigger is contagious, not per-command

Original timeline: a `find | sort | xargs sha256sum` over ~15k files wedged first (a
fork-churn storm), then a ~20-minute burst of wedges at high rate (8/8 `diff`, 2/2
`cmp`), then — after force-killing every wedged process — thousands of clean children
again. That matches the 2,400 and 3,640 clean runs in the follow-up: both ran in a
clean-state system. Also in the background history before the first wedge: several
process trees killed mid-flight with job-object terminate. Suggested seeding:

* fork churn first (the `final-verify.sh` fork-churn step, `xargs -P` pipelines over a
  big tree, concurrent loops);
* kill some children mid-run between attempts — candidate vector: pipe mutex /
  shared-object state left behind, the exact neighborhood of the "mutex re-create
  recovery in `raw_write`" change;
* run while the machine is busy (the original session had Defender scanning tens of
  thousands of fresh generated files, ~150k system handles, many resident processes);
  per FORK_FAILURES.md the reservation failure that feeds the retry machinery needs
  memory pressure and occupants;
* once the first wedge appears, **keep the loop running and capture the next one** — do
  not clean up between attempts; the wedged processes themselves appear to be part of
  the trigger state (contagion), so a loop that resets to clean state will under-count.

Deterministic alternative for exercising the suspect retry path on every fork: a helper
process that reserves/fragment random ranges inside the `CYGHEAP_STORAGE_LOW` 8 GiB
window, forcing `cygheap_fixup_in_child`'s fixed-address reservation to fail
every time. Per FORK_FAILURES.md that is exactly the failure that routes into the
checked-retry/diagnostics code — the prime H1 suspect.

### 3. Topology traps (verified NOT to reproduce, in-session)

* `strace diff a b` — strace spawns the program via CreateProcess; the fork/exec
  emulation path is bypassed and the child is always healthy. Use `strace -p` only for
  after-the-fact capture.
* `powershell Start-Process diff` — native spawn, always healthy.
* suspect command as the *last* command of a script (bash may exec it directly without
  forking) — use intermediate commands and `( … ) &` subshells.
* child stdio redirection is irrelevant (children wedged with `>/dev/null`, with file
  redirection, and with pipes alike); it is the **parent's** pipe fhandlers that the
  fork path sees.

### Owner evidence without instrumentation (answers "instrumentation blocked")

None of these perturb the build, so the Heisenbug concern does not apply:

* **Lock word**: `msys-2.0.dll base + RVA(_ZN12init_cygheap15cygheap_protectE)` —
  resolve the RVA per build with `nm` (build-specific: `0x289d08` in the released
  v0.4.0/Nov 3 build, `0x28c520` in the ring build). Healthy `0x0`; wedged `0x…3`
  (writer-held + waiters, wait block on the wedged thread's own stack). Reader scripts
  `scan4.ps1` / `scan4b.ps1` are in the evidence artifacts below.
* **`strace -p <pid>` attach** retroactively dumps the wedge's in-memory trace —
  including the transient thread `created → exited 0x0` right before the silence. The
  strace ring is part of the stock runtime; nothing is rebuilt.
* **`NtQueryInformationThread (ThreadQuerySetWin32StartAddress)`** on the terminated
  thread of a freshly wedged process names its start routine — run this before killing
  the process.
* **Hardware watchpoint as zero-code-change tracer**: a tiny debugger process
  (`DebugActiveProcess` + `DebugSetProcessKillOnExit(false)`) that sets a write watch
  `Dr0 = &cygheap_protect` on every thread via `CREATE_THREAD_DEBUG_EVENT` →
  `SetThreadContext`. Note `0x7ffffb203` is consistent both with a genuine SRW
  wait-queue state (orphaned owner — H1) **and** with "lock word corrupted to nonzero,
  then the waiter queues normally"; the write watchpoint is the cleanest way to tell
  orphaned-owner from a stray store (which would also fit the ring build shifting
  `cygheap_protect` to a new RVA and changing behavior).
* **Canaries**: `diff`/`cmp` were the highest-rate triggers in the original burst (8/8
  and 2/2 wedged while `git diff --no-index` completed in the same second). Loop:
  `( diff a.tmp b.tmp; echo $? > m ) &`. The diffutils bias may be cluster noise — mix
  in `cat`/`md5sum`/`printf` children to test breadth.

## Summary

Running a **pig** session (PiG — the coding-agent harness defined under "Reproduction
tips" below; it runs model-requested shell commands through a sandboxed bash tool) on
DEN-PC with the patched `msys-2.0.dll` drop-in
(Nov 3 17:06, installed over `C:\Program Files\Git\{bin,usr\bin}\` by `tools/ci/install.ps1`),
children created by `bash` via the fork/exec path intermittently hang permanently. The hang
is:

* in the **child's first cygheap allocation** (`open()` → `mount_info::conv_to_win32_path`
  → `_cmalloc`), blocked in `AcquireSRWLockExclusive (&init_cygheap::cygheap_protect)`;
* **untouchable from inside MSYS**: `timeout 3 diff a b` ran 37 minutes past its deadline;
  MSYS `kill`/`TerminateProcess` via the runtime does not reap it (native
  `Stop-Process` does);
* **contagious / clustered**: while wedged children exist, subsequent fork/exec children
  wedge at elevated rates; after force-killing every wedged process, the *same commands*
  immediately work again (`diff`, `cmp` → rc=0).

The parent job never completes, so from the harness side it looks like a "stuck background
job"; it is really a dead-locked child.

## Observed victims and controls

| Wedged (fork/exec children of bash) | Healthy (same topology) |
|---|---|
| `diff.exe` (8/8 attempts), `cmp.exe` (2/2) | `cat`, `cp`, `od`, `tr`, `md5sum`, `printf`, `ls`, `grep`, `sed`, `sort`, `awk`, `sleep`, `git diff --no-index`, `cmd`, `powershell`, `cargo`, `rustc` |
| one `find \| sort \| xargs sha256sum` pipeline (no output after 8 min) | |
| one `sha256sum` of 5 files | |

Notable simultaneity: in one probe, `diff` and `cmp` wedged while `git diff --no-index`
finished rc=0 in the same second (same files, same shell). A later `cmp` that had been
wedged for ~30 min eventually died with `cmp: ta.tmp: Bad address` (EFAULT — corrupted
child state). Whether diffutils is genuinely more exposed or this was clustered
probability is an **open question** (see below).

Stacks of all wedged processes were identical in shape. Example (PID 21132 main thread,
poor-man's walk, symbols via `tools/git-priv/bin/msys-2.0.dll` — unstripped twin of the
installed build, same `ImageBase 0x210040000`):

```
ntdll!NtWaitForAlertByThreadId                      (ntdll+0x16a054)
ntdll!RtlpWaitOnAddress …                           (ntdll+0x61e2b, +0x16ab3f)
msys-2.0!open                         syscalls.cc:1488   (msys-2.0+0xa020a)
msys-2.0!&init_cygheap::cygheap_protect  cygheap.cc:38   (msys-2.0+0x289d08, the lock arg)
msys-2.0!_cmalloc                     mm/cygheap.cc:390  (msys-2.0+0x15017f)
msys-2.0!mount_info::conv_to_win32_path mount.cc:737     (msys-2.0+0x3f243)
msys-2.0!creturn                      mm/cygheap.cc:454  (msys-2.0+0x150a63)
msys-2.0!path_conv::set_path          path.h:441         (msys-2.0+0x5c746)
```

Secondary threads are parked normally (`wait_sig` sigproc.cc:1498,
`cygthread::callfunc` cygthread.cc:130). **Every wedged process also contains exactly one
terminated thread** (`GetThreadContext` fails on it). `strace -p <pid>` on a wedged child
flushed its runtime log: DLL loads complete, then **one transient thread is created and
exits `0x0` immediately**, then the trace goes silent until the process is killed.

## The lock word (decisive datum)

`init_cygheap::cygheap_protect` is a `static SRWLOCK` in the DLL's data section
(`mm/cygheap.cc:38`, `local_includes/cygheap.h:502,546`: `AcquireSRWLockExclusive` /
`ReleaseSRWLockExclusive`). Symbol RVA in this build: **`0x289d08`**. Live values read
with `ReadProcessMemory` at `msys-2.0.dll base + 0x289d08`:

| Processes | Lock word |
|---|---|
| 8 wedged `diff` children | `0x00000007ffffb203` (×7), `0x00000007ffffc1c3` (×1) |
| 16 running `bash` + a healthy forked `sleep` child | `0x0000000000000000` |

`0x…b203` decodes as RTL_SRWLOCK `Ptr = waiter-wait-block | 0x3` — i.e. **held exclusive
with waiters, the wait block living on the wedged thread's own stack** (its RSP was
`0x7ffffb1d8`). So the child is queued behind a holder that will never release: the lock
was already in "held" state when the child's main thread reached its first allocation.

## Possible cause (hypotheses)

**H1 (superseded) — a transient child thread exits holding the lock.** During child init the
patched runtime creates a short-lived thread (visible in the strace log: created → exited
`0x0` before the wedge). If that thread takes `cygheap_protect` and then exits (or blocks
indefinitely) without `ReleaseSRWLockExclusive`, the lock is orphaned and every later
allocation dead-locks. Nothing in the tree re-initializes `cygheap_protect` on the child
side (exactly three references exist: definition, declaration, lock/unlock inline) — that
is fine upstream, where children start quiescent and the lock guards only short
allocations, but it leaves zero tolerance for any new code path that acquires it and can
bail out. Candidates in this patch's inventory: the fork/`child_copy` retry machinery,
`proc_retry`-driven child-death handling, and the pipe `need_fork_fixup` cross-process
handle re-dup (`fhandler/pipe.cc`, `fork.cc`, `mm/cygheap.cc` per [STATUS.md](STATUS.md)).

**H2 (superseded) — copied "held" state from the parent.** The fork child inherits the parent's data
section; if any patched path makes the parent (or a parent-side helper thread) hold
`cygheap_protect` while `frok::parent` copies, and the copy is not excluded/reset (the
`NO_COPY` marker must be honored by the chunked `child_copy` retries!), the child waits on
a holder that does not exist in its address space. H2 also predicts the exact observed
lock word.

**H3 (remaining open question) — contagion mechanism for the original clustered bursts.** Wedged
processes raise the wedge rate for new children; killing them all restores normal
operation immediately. Plausible carriers: shared kernel objects touched by the patched
pipe/named-mutex recreate logic (`shared.5` user info, pipe mutexes), or fixed-address
cygheap reservation pressure (`CYGHEAP_STORAGE_LOW` 8 GiB window) feeding the retry
budget described in FORK_FAILURES.md. Needs its own stress experiment to characterize.

## Reproduction

Fastest faithful vehicle: the **piped-prompt pig invocation** in §1 of the reproduction
tips above. Raw form, through a bash where `msys-2.0.dll` is the patched drop-in (the
harness-spawned `C:\Program Files\Git\bin\bash.exe` showed it most reliably):

```sh
printf x > a.tmp; printf x > b.tmp
( diff a.tmp b.tmp; echo $? > m1 ) &     # m1 appears only if the child survives
( cmp  a.tmp b.tmp; echo $? > m2 ) &
sleep 10; ls m1 m2                       # wedged children: marker missing forever
```

Once wedges are present they cluster; native `Stop-Process -Force` on the wedged
processes clears the condition. `timeout N` cannot kill a wedged child.

## Suggested next steps

1. Instrument `init_cygheap::lock/unlock` (entry/exit + `GetCurrentThreadId`) and thread
   create/exit in children, log to a file; run the fork-churn step of
   `tools/final-verify.sh` under load until a wedge appears. The first thread to take the
   lock and never release it names the guilty path.
2. Identify the transient thread (start address via `NtQueryInformationThread
   (ThreadQuerySetWin32StartAddress)` on the created-then-exited tid).
3. Belt-and-braces hardening regardless of root cause: reset
   `init_cygheap::cygheap_protect = SRWLOCK_INIT` in the child fixup path
   (`cygheap_fixup_in_child`), and audit every new acquisition site for
   early-return/`ExitThread` paths between `lock()` and `unlock()`.
4. Stress loop to settle the "diffutils-biased?" question (binary image size / argv /
   fhandler mix vs pure clustered probability).

## Side findings from the same session (for the record)

* **`gdb` went silent mid-investigation**: Windows Defender quarantined `gdborig.exe`
  (the real debugger behind the 58 KB `gdb.exe` shim) at 21:27:16–49 with
  `Trojan:Win32/Bearfoos.A!ml` (ID 2147731250), triggered by our process-attach attempts.
  Restore needs admin: `"C:\Program Files\Windows Defender\MpCmdRun.exe" -Restore -All`
  or an exclusion for `C:\ProgramData\mingw64`. (Workaround used: SuspendThread +
  GetThreadContext + stack scan from PowerShell.)
* **Agent-harness `fs::remove_dir_all` stale-tree episode**: a generator's silent
  `let _ = remove_dir_all(out_dir)` left 1063 stale files across two regenerations.
  Retested after the sandbox was disabled and all wedged processes (which can hold
  handles) were gone: deletion works (`Ok`). Environmental, not a generator bug — but the
  swallowed error should still be propagated.

## Evidence artifacts

In `C:\Users\<user>\workspace\stm32-data\` (session cwd):

* `scan3.ps1` — thread context + stack scanner (poor-man's stack walk).
* `scan4.ps1` / `scan4b.ps1` — live SRWLOCK word readers (wedged / healthy controls).
* `msyms.txt` / `msyms2.txt` / `msyms3.txt` — symbol dumps of the unstripped twin DLL.
* `st-spawn.txt` — full strace of a *healthy* `diff` run (strace-spawned topology).
* `defender-log.txt` — Defender operational log excerpt (quarantine events).
* `gdb-stack.gdb`, `rmtest2.rs`, `scan2.ps1` — supporting tools from the hunt.
