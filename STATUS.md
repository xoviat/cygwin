# STATUS — git-bash inside the mxc ProcessContainer — **GOAL VERIFIED**

```
$ bash tools/final-verify.sh
==================== RESULT: 11 passed, 0 failed ====================
GOAL VERIFIED: git bash works inside the mxc sandbox.
```

`git bash` runs via `mxc_sdk::v1::run` (ProcessContainer backend), empirically
verified through the whole chain: cygwin `winsup/` fixes (this repository) →
`tools/ci/cygwin-winsup.patch` → `tools/ci/apply-to-msys.sh` →
git-for-windows/msys2-runtime → patched `msys-2.0.dll` in a private Git copy →
mxc runner. The gate is re-runnable and was green twice in a row, including
once after all diagnostic code was stripped.

## 1. The root cause chain (microsoft/mxc#1061, buhuikongpan/dsh-win-gitbash#10)

1. **`_everyone_sd` (sec/helper.cc) granted only World (S-1-1-0).**
   AppContainer processes are *not* covered by "Everyone": object opens from
   inside the container need an **ALL APPLICATION PACKAGES (S-1-15-2-1)** ACE.
2. → `cygwin_pid()` resolves winpid→cygpid via a `winpid.<N>` symbolic link;
   the parent's `NtOpenSymbolicLinkObject` was denied (`0xC0000022`) →
   `cygwin_pid` returned 0.
3. → `frok::parent` returned `child_pid = cygwin_pid(pi.dwProcessId)` = **0** —
   the parent's `fork()` returned 0 and ran the child branch.
4. → both processes ran pipeline element 1 (`bash -x` proved `+ echo hi`
   twice, `tr` never), script tails were lost, and the pipe's server (read)
   end closed everywhere.
5. → the named-pipe instance died; client-end writes failed `0xC00000B1`
   (`ga=0x120196` intact — the "object degradation" mystery was instance
   death) → `echo: write error` → `Exited(3328)`.

**The fix (the missing ACE):** `_everyone_sd` now also allows ALL APPLICATION
PACKAGES; `everyone_sd()`'s alloca buffer grew to 2 ACEs. With it `fork()`
returns real pids and pipelines work end-to-end.

## 2. Fix inventory (all in winsup/cygwin, shipped as `tools/ci/cygwin-winsup.patch`)

| Area | Files | What |
|------|-------|------|
| Confinement DACLs | `sec/helper.cc`, `local_includes/security.h` | ALL APPLICATION PACKAGES ACE in `_everyone_sd` (the root fix); `sec_acl` grants the confined token's logon SID + AppContainer package SID (restricted-token clients, dsh#10 signal pipes); `seteuid`'s default-DACL buffer widened to `MAX_DACL_LEN(7)` (`syscalls.cc`) |
| Namespace fallback | `mm/shared.cc` | shared-object parent dirs fall back `BaseNamedObjects → session BNOLINKS → \BaseNamedObjects\ … → AppContainer private namespace` (`"AppContainer ns: %y"` chain) |
| Named mappings | `mm/shared.cc` | DACL-less sections get an explicit everyone-ACE SD (hardened sandboxes rewrite NULL DACLs and then deny siblings); borrowed-handle mapping falls back to name-based reopen; `user_info::create` always derives the name |
| Pipes | `fhandler/pipe.cc` | absolute NT pipe names (opening the NamedPipe root is denied in-container), CreatePipe-anonymous `\Device\NamedPipe\pipe-%u-` naming (other names' handles get dropped across inheritance), create without OBJ_INHERIT + `SetHandleInformation` afterwards, `need_fork_fixup(true)` (cross-process re-dup of pipe handles), `sec_user` SA for pipes, mutex re-create recovery in `raw_write` |
| Fork/wincap | `wincap.cc`, `local_includes/shared_info.h`, `tty.cc` | `in_this_image` cross-image wincap guard (PSEC loads per-process copies; "fork-friendly" caps must come from the real kernel), shared-info hardening, tty fixes |

**Mandatory companion step (not a source fix):** the mxc PSEC force-relocates
images that have not opted into ASLR, breaking cygwin's same-address fork.
Every runtime DLL/exe must have the PE `DYNAMICBASE` flag set —
`python tools/ci/peflag.py set <install-dir>` (walks *.exe/*.dll; the gate
does this automatically).  Drop-in use: copy the released `msys-2.0.dll`
over a Git for Windows install's `{usr/bin,bin}/msys-2.0.dll`, run
`peflag.py set` on the install, done — verified on a pristine copy
(bash --version, pipelines, file IO all green in the mxc sandbox).

## 3. Layout, build, verify

- This repo = the cygwin tree; the fixes live in `winsup/cygwin/`.
- `tools/ci/cygwin-winsup.patch` — the canonical patch (generated with
  `git diff -- winsup/`).
- `tools/ci/apply-to-msys.sh <msys2-runtime-src>` — transfers it to a clean
  git-for-windows/msys2-runtime checkout (pinned in `tools/ci/MSYS2_RUNTIME_REF`).
  Verified byte-identical round-trip against the gate-tested tree for all 8
  patched files.
- `tools/ci/peflag.py` — PE ASLR opt-in tool (stdlib-only Python).
- `tools/ci/mxc_gitbash_run.rs` — the runner example (dropped into
  microsoft/mxc `src/mxc-sdk/examples/`, pinned in the workflow).
- `tools/final-verify.sh` — the gate. `MSYS_DLL=<path>` overrides DLL lookup;
  defaults suit the local dev layout (`tools/msys2-runtime/build-cy`,
  `tools/git-priv`, `tools/run/mxc_gitbash_run.exe`). It never touches the
  real `C:\Program Files\Git`.
- msys DLL build (both locally and in CI): root
  `configure --build=x86_64-pc-cygwin … build_alias=x86_64-pc-cygwin`, then
  `make`. cygwin1.dll build: same configure, `make all-target-newlib`, then
  winsup configured with `--disable-doc` and
  `make child_info_magic.h shared_info_magic.h globals.h localtime.patched.c`
  before `make new-cygwin1.dll` (generated-headers dependency gap).

## 4. CI (`.github/workflows/verify-and-build.yml`)

1. **verify** (windows-latest): checkout → setup-msys2 (mingw-w64-cross-gcc
   toolchain) → checkout msys2-runtime @ pinned → `apply-to-msys.sh` → build
   `msys-2.0.dll` → build the mxc runner example (Rust) → robocopy
   `C:\Program Files\Git` to `tools/git-priv` (copy only) → run
   `tools/final-verify.sh` (the gate must end `RESULT: 11 passed, 0 failed`).
2. **build-dlls** (after verify): build `cygwin1.dll` from this repo's winsup,
   collect both DLLs, `peflag.py set` them, upload as `runtime-dlls`.
3. **release** (tag pushes only): attach the DLLs to the tag's GitHub release
   (`softprops/action-gh-release`, creates the release if absent). Fallback:
   the `runtime-dlls` workflow artifacts stay downloadable from every run.

## 5. Loose ends (known, non-blocking)

0. **CI host support for the ProcessContainer** (established by the gate's
   raw output): the backend needs **Windows PSEC**, which GitHub's
   Server-SKU images (windows-2022/2025, build 26100) do not enable --
   "Windows ProcessContainer is unavailable: PSEC is not enabled".  The
   gate therefore probes with cmd.exe and SKIPS the in-sandbox section
   there (builds, patch transfer and outside-sandbox behavior are still
   verified).  On `windows-11-arm` (the only client-SKU hosted image,
   build 26200) PSEC works: bash --version and builtins pass in-sandbox,
   but fork/exec-dependent tests fail (`dofork: forked process died
   unexpectedly`) -- x64-on-ARM emulation cannot reproduce cygwin's
   fixed-address fork reconstruction (no ARM64 cygwin exists yet; see
   Windows-on-ARM-Experiments/newlib-cygwin).  That entry is
   informational; the full gate needs a PSEC-enabled x64 Windows host.

1. `git --version` inside the sandbox: `/bin/git.exe` exists and `/bin` is on
   the workload PATH, but bash's PATH search does not resolve `git` (`type git`
   → not found). Everything else in the gate works; likely a small exec-search
   or policy-path nuance. The gate does not cover `git` itself yet.
2. `frok::parent`'s "died unexpectedly" retry loop does not TerminateProcess
   the ghost child (stock semantics; two `sleep.exe` observed) — worth an
   upstream fix on its own.
3. `child_info::parent_winpid` is never set in the fork path (only
   `spawn.cc` sets it); fork children read uninitialized stack there.

## 6. History

Root cause and fix were established by instrumenting the msys runtime
(TEMPORARY DIAGNOSTIC code, all stripped before this branch): name-timeline
dumps showed pipe instances dying after `close` of the last server handle;
`frok::parent return child_pid=0` pinned the fork bug; `cygwin_pid`'s
symlink-lookup denial (`0xC0000022`) located the missing ACE. The debugging
workflows (hardware breakpoints across fork, process freeze, handle
inspection) are described in the upstream issue write-ups.
