# Fork failures: reproduction cases

> **RESOLVED (2026-10-31) — fixed in the runtime, kept under gate.** See
> [Resolution](#resolution-2026-10-31-root-cause--fixes) below; the
> reproduction cases in this file are kept as the historical record and as
> the scenario `tools/final-verify.sh`'s fork-churn step now covers.

Observed while running this session's work (2026-10-29, roughly 16:00–21:30
local) through an agent harness on Windows. The failures are transient,
process-level, and hit **bash child-process creation** (`fork` inside MSYS/
Git-Bash), not any project code. This file collects the exact signatures, the
commands in flight when they occurred, and concrete cases to reproduce them.

## Resolution (2026-10-31): root cause & fixes

### Root cause (confirmed in code, matches every observed field datum)

The failure chain is entirely inside the forked child's early DLL init:

1. `cygheap_fixup_in_child` (mm/cygheap.cc) reserves the cygheap at its
   **fixed** address (`CYGHEAP_STORAGE_LOW` = `0x800000000`, 8 GiB window) and
   commits up to `cygheap_max`.  Under the mxc ProcessContainer this
   `VirtualAlloc` **fails intermittently** (`Win32 error 487 /
   ERROR_INVALID_ADDRESS` even though the window base reads `MEM_FREE` — a
   partial occupant *anywhere* in the 8 GiB range fails the whole
   reservation while a query at the base alone looks clean).  The failure
   is per-process random (~1–2% of children under churn, clustering in
   bursts) and hits **both fork children and exec/spawn children** — which
   is why retrying the identical command always fixed it.
2. The failure was **invisible**: the NULL `cygheap` pointer was fed to
   `child_copy (…, "cygheap", cygheap, cygheap_max, …)`, which read the
   parent from address `0x0` → `ERROR_PARTIAL_COPY` (299) with `done 0` —
   exactly the reported `child_copy: cygheap read copy failed,
   0x0..0x800021040, … Win32 error 299` line (the `0x0` low address is the
   NULL `cygheap`, the `0x80002xxxxx` high address is the parent's
   `cygheap_max`).
3. The child then died in a NULL dereference during process initialization,
   which Windows reports as `STATUS_DLL_INIT_FAILED` (`0xC0000142`) — the
   parent's `dofork:` line.
4. The parent **gave up immediately**: `frok::error` only reached
   `child_info::proc_retry` when the exit code was zero — i.e. never for a
   child that actually died — so the designed `STATUS_DLL_INIT_FAILED`
   retry machinery in `proc_retry` never fired (hence `retry 0`), and its
   budget (`CYGWIN=proc_retry`) defaults to 0 anyway.  → `fork` returned
   `EAGAIN`, bash printed `fork: retry: Resource temporarily unavailable`,
   and pipelines/heredocs lost data (cases 4–6).

### Fixes (winsup/cygwin)

| Area | Change |
|---|---|
| `mm/cygheap.cc` | `alloc_cygheap_in_child`: brief retry around the fixed-address reserve/commit; **all results checked**.  On failure: a real diagnostic (the loud form walks the whole window with `VirtualQuery` and names the occupant, incl. the loaded module for `MEM_IMAGE`) and a **retryable** death (`EXITCODE_RETRY`) instead of a NULL deref. |
| `fork.cc` `child_copy` | bounded chunked copies with per-chunk retries (tolerates transient `ERROR_PARTIAL_COPY` while a sandbox scans memory mid-copy); a read-side failure now kills the child with `EXITCODE_RETRY` (it must not continue half-copied) instead of `TerminateProcess` on the parent handle; quiet on retryable attempts, loud when the budget is spent. |
| `child_info` (`sigproc.cc`, `child_info.h`) | the `proc_retry` budget is now seeded for **fork, spawn and exec** children (`CHILD_INFO_RETRY_BUDGET` = 10, `CYGWIN=proc_retry=N` overrides).  Previously the budget was 0 and exec/spawn children got exactly one attempt — the source of the workload's data loss even after fork-level retries. |
| `fork.cc` `frok::parent` | child deaths are routed through `child_info::proc_retry` so `STATUS_DLL_INIT_FAILED`/`EXITCODE_RETRY` consume the retry budget and the fork is repeated with a fresh child (forkables re-armed between attempts, abandoned sync-timeout children terminated); spawn/exec retries run through the same budget via `spawn.cc`'s existing `proc_retry` loop. |

With the fixes, a transient child-init death is invisible to the workload:
the same `fork()` simply succeeds after a few attempts.  Loud diagnostics
(`cygheap: VirtualAlloc …`, `… copy failed`) appear only when the retry
budget is actually spent.

### Gate coverage

`tools/final-verify.sh` (the gate CI runs on every push) gained a
**"sustained fork churn"** step: the workload shape from
`tools/repro-fork/workload.sh` (pipelines, chained pipelines, heredoc
integrity, command substitution, concurrent forks) inside the sandbox,
classified on the verbatim signatures (`child_copy:`, `dofork:`, `copy
failed`, `VirtualAlloc`, `Resource temporarily unavailable`, `0xC0000142`,
`-1073741502`) **and** on the workload's own integrity markers — not on the
exit code alone.  Verified to fail with the pre-fix DLL (the churn load
reproduces the `5/5` repro-round failure rate at `CHURN_N=100`) and pass
with the fixed build.

## Environment

- **Shell engine**: `C:\Program Files\Git\bin\bash.exe` (Git for Windows / MSYS2,
  msys-2.0.dll fork emulation). The session's `shellPath` setting resolves to
  it.
- **Harness**: agent session executing bash commands under the **MXC
  container/sandbox** (`mxc#1061`), workspace-write mode.
- **Related context**: the *very first* tool call of this session was rejected
  outright by the harness with:

  ```text
  SANDBOX_UNAVAILABLE: the resolved shell is an MSYS/Git-Bash engine
  (C:\Program Files\Git\bin\bash.exe), which cannot start inside an MXC
  container (mxc#1061 — the MSYS object namespace is denied; no configuration
  works around it).
  ```

  Subsequent calls ran after escalation/approval, i.e. the whole session runs
  an MSYS shell in a combination the harness itself documents as unsupported.
  **This unsupported shell+container pairing is the primary suspect.**

## Error signature (verbatim)

All occurrences share this shape (PID/addresses vary; `retry N` increments):

```text
      0 [main] bash (29096) child_copy: cygheap read copy failed, 0x0..0x800021040, done 0, windows pid 29096, Win32 error 299
      0 [main] bash 1203 dofork: child -1 - forked process 29096 died unexpectedly, retry 0, exit code 0xC0000142, errno 11
/usr/bin/bash: fork: retry: Resource temporarily unavailable
```

Field notes:

| Field | Observed values | Meaning |
| --- | --- | --- |
| `child_copy: cygheap read copy failed` | always | MSYS fork could not copy the parent's cygwin heap into the child |
| region end | `0x800020D60`, `0x800020EF0`, `0x800021040`, `0x800021180`, `0x800021270` | varies slightly per process (heap size) |
| `Win32 error` | **299 always** | `ERROR_PARTIAL_COPY` — ReadProcessMemory into the child got only part of the range (child dying mid-copy, or a third party mapping/unmapping its memory) |
| child `exit code` | **`0xC0000142` always** | `STATUS_DLL_INIT_FAILED` — child died during DLL initialization |
| `errno 11` | always | EAGAIN → parent then prints `fork: retry: Resource temporarily unavailable` |
| tool exit code on hard failure | **`-1073741502`** | = `0xC0000142` as the process exit code of the whole bash tool call |

Prefix variants seen: `0 [main] bash (PID)` and `0 [main] bash PID` and
`[main] bash NNNN dofork:` — cosmetic (msys event formatting), same failure.

## Observed cases (chronological, this session)

Two outcome classes:

- **Self-recovered**: the fork error lines printed, but bash's internal fork
  retry loop succeeded on a later attempt; the command's real output followed.
- **Hard fail**: retries exhausted (or the parent itself was poisoned) and the
  whole command died with exit `-1073741502` (0xC0000142).

| # | Command in flight | Outcome |
|---|---|---|
| 1 | `cd internal/dap && rg -n "steerSignal\|SteerSignal\|steer" session.go protocol.go \| head -20` | **self-recovered** — fork error for one pipeline stage, rg results still printed |
| 2 | `cd internal/dap && python _strip_steer.py && rm _strip_steer.py && echo STRIPPED` | **hard fail**, exit `-1073741502`, no output. Immediate retry of the same command succeeded |
| 3 | `python - <<'EOF' …EOF` (heredoc script via stdin) | **ambiguous**: python started in interactive REPL mode (`Python 3.15.2 … >>>`, `warning: can't use pyrepl: [WinError 6] The handle is invalid.`) instead of executing the piped script — consistent with the pipe/fork machinery misbehaving; may also have been a command-construction bug |
| 4 | `grep -o 'LATEST_PROTOCOL_VERSION="[^"]*"' <pi-mcp\client.js> \| head -3; grep -o 'SUPPORTED…' …` (chained greps) | **hard fail** after fork lines; only `PROTOCOL_VERSION…` output from the first stage |
| 5 | `rg -o 'PROTOCOL_VERSION[^,;]*' <client.js> \| head -5; rg -o '2025-…' …` (two-stage pipeline) | fork failure between stages; second stage produced no output |
| 6 | `cat >> session_test.go <<'EOF' … EOF` followed by `gofmt … && go test …` | **hard fail**, exit `-1073741502`. **Side effect: the heredoc content was lost** — `session_test.go` was unchanged afterwards (verified with `rg -c`). Re-applied via a different edit path |
| 7 | `grep -c PROTOCOL <client.js>` | **self-recovered** — `child_copy … 0x800021270 … retry 0` then the answer (`3`) printed |
| 8 | `cli mcp add -l dap-test -- ./… && cli mcp list 2>&1 \| tail -30` (chained CLI subcommands) | **self-recovered** — fork failure between the two CLI invocations (`bash (29096)`), the `list` subcommand still ran afterwards |
| 9 | `cd …/pi-mcp && grep …` | *different failure*: `cd: …: Permission denied` — MSYS object-namespace denial (mxc#1061), same root area, no fork involvement |

Failure-rate context: over a ~5.5 h session with several hundred tool calls,
cases 2, 4, 5, 6 were hard fails (≈1%) and 1, 7, 8 were visible-but-recovered.
Every hard fail was fixed by simply **retrying the identical command**.

## Correlations observed

1. **Process churn immediately before the failure.** Cases 2/4/6 followed
   heavy child-process activity in the preceding calls: `go test -race ./…`
   (each test run spawns the mock-adapter grandchild processes), `go build`,
   repeated CLI tool launches (case 8 is literally between two CLI spawns).
2. **Pipeline/heredoc commands** (cases 1, 4, 5, 6, 7, 8) — every pipeline
   stage and command substitution is a fork; simple builtins were never hit.
3. **Long-lived session** — first failure appeared ~30–60 min in; the parent
   bash process accumulates cygheap/state over hundreds of forks.
4. **Sandbox/harness context** — all calls run inside the MXC sandbox with
   process-level instrumentation; the harness itself flags this shell as
   unsupported in the container.

## Ranked hypotheses

1. **MSYS fork emulation × MXC sandbox instrumentation** (most likely):
   the sandbox injects/hooks child processes; the forked child's DLL-init runs
   with the copied cygheap while the sandbox's own DLL/module initialization
   interleaves → partial copy (299) and/or DLL init failure (0xC0000142).
   The harness's own `mxc#1061` message corroborates that MSYS + MXC is a
   known-bad combination.
2. **Antivirus interference during DLL init** (e.g. Defender real-time scan of
   `bash.exe` children): classic cause of `0xC0000142` at process start, and
   would produce intermittent `ERROR_PARTIAL_COPY` while AV reads child memory.
3. **Resource pressure in a long-lived parent**: handle/memory growth over
   hundreds of forks; EAGAIN is the documented MSYS fork-retry exhaustion
   signal. (Weaker: it never failed twice in a row and pressure was not
   measured.)
4. **Git-for-Windows/MSYS bug version-specific**: cygheap layout at
   `0x80002xxxxx` and fork-copy races fixed in some msys-2.0.dll builds.

## Debugging notes (reproduction campaign, 2026-10-30)

Field data from building and running `tools/repro-fork/`. Updates the
hypotheses above with hard evidence; ordered as a decision trail.

### What is now established

1. **The failure is in the container, full stop.** Same bash.exe, same
   `msys-2.0.dll`, same workload script, same host: in-container 5/5 rounds
   hard-fail at n=500 (and 3/3 at n=100); the unconfined control is clean
   (rc=0, `workload done fails=0`, zero signatures) at the same n. The
   container containment is the **only** variable that differs.
2. **The patched DLL does not fix it.** The live
   `C:\Program Files\Git\usr\bin\msys-2.0.dll` (sha256
   `cc76de65…b186cc`) is byte-identical to release v0.1.0's patched build
   (carries `AppContainer ns: %y` + `in_this_image`). This reproduces the
   **residual** fork failure that survives the `_everyone_sd` ACE fix — i.e.
   the mxc#1061 fork-returns-0 fix and this failure are independent bugs.
3. **No long session / no churn needed → hypothesis 3 is out.** First
   signature appears within seconds of the first round (n=5: 4/10 rounds;
   n=500: signatures from round 1). No hour of accumulation, no `go test`
   churn, no handle pressure required. EAGAIN (`errno 11`) is the *parent's
   retry loop reporting the dead child*, not resource exhaustion.
4. **Antivirus is not required → hypothesis 2 is out (as a cause).** Defender
   scans the identical files in the clean control runs; something about the
   container makes the child die *during* DLL init deterministically.
5. **It is not a specific-binary bug → weakens hypothesis 4's "version-
   specific race" framing.** In-container the signatures also fire for `cat`
   and `seq` — any MSYS image forking inside the container is exposed; the
   `0x80002xxxxx` cygheap range is the same for all of them in a round.
6. **Hypothesis 1 stands, narrowed**: something in ProcessContainer creation
   of the child *interleaves with the child's own DLL initialization while
   the parent is still copying the cygheap* — consistent with 299
   (`ERROR_PARTIAL_COPY`: the copy source/destination changed or child died
   mid-copy) always paired with 0xC0000142 (child's DllMain failed). The
   parent observes the child dying mid-DLL-init and reports exactly that.
7. **Gate tests still pass in-container (9/9)** — a handful of forks
   (`echo hi | tr`, a 3-iteration loop) succeed; failure probability is per
   fork and only becomes near-certain under churn (see rates below). This is
   why the original session saw ~1% transient failures while a stress loop
   hits every round.

### Measured rates (reproducer `repro.sh`)

| n (fork-loop size) | Rounds | Result |
|---|---|---|
| 500 | 5 | 5/5 hard-fail, many signatures per round |
| 100 | 3 + 3 | 6/6 hard-fail |
| 50 | 1 | signature present |
| 5 | 10 | 4/10 rounds show signature (intermittent per round) |

⇒ Per-fork failure probability is high in-container; round-level results
scale with fork count. The unconfined control at n=500 had **0** signatures.

### Signature variants seen beyond the original report

- `child_copy` fires for non-bash images too (`cat (26064)`, `seq (31648)`) —
  the mechanism is msys-2.0.dll fork, not bash.
- Some `child_copy` lines appear **without** a following `dofork:` (the
  failing child was a grandchild; its parent's own error propagated to the
  shared console) and some `dofork:` lines carry a large timestamp-style
  prefix (`3724165 [main] bash …`) — cosmetic event formatting, same event.
- Observed region ends in-container: `0x800022020`, `0x800022330`,
  `0x8000224D0`, `0x800022D70`, `0x8000230C0`, `0x800023180` — same
  `0x80002xxxxx` band as the original report, grows with heap size.
- When bash's internal fork-retry exhausts, the *workload loses data*
  (reproduced: chained-pipeline second stage silent, heredoc file never
  written) — the operational consequence of case 5/6.
- The runner's own container `outcome` may still read `Exited(0)`/non-zero
  depending on which phase absorbed the fork loss — classify on the stderr
  signatures, not the exit code alone (`repro.sh` does both).

### Environment deltas vs the original session

- Original: failures inside a long-lived agent session, several hundred tool
  calls, workspace-write sandbox, first failure ~30–60 min in.
- Reproducer: fresh container per round, standalone mxc runner (no agent
  harness at all), failure from round 1. Same host, same shell, same DLL.
- Host: Windows 11 build 26300.9711 client SKU (PSEC enabled), Git for
  Windows 2.56.0.windows.4 / bash 5.3.15, mxc @ `05a430bc` (mxc-sdk 1.0.1),
  msys2-runtime base `710e5275`.
⇒ The agent session's "long-lived parent" and "harness instrumentation"
   factors are **not necessary** for the failure (they may still modulate
   rate; the original ~1% per command vs reproducer's ~100% per round at
   n=500 reflects per-fork probability × forks-per-command).

### Oddity noted while packaging — same family, fixed by the resolution

A relocated copy of the Git install (`tools/git-priv`, made for the gate)
execs with **silent no-output and exit 127/0** when spawned from an MSYS
parent process in this environment, but runs correctly via `cmd`/`powershell`
*and* inside the container. This turned out to be the same bug in a
different costume: a cross-copy exec (parent and child use *different*
copies of the runtime DLL) runs the spawn-side `child_copy` from the parent
while the sandbox/ASLR machinery churns, and the old non-retrying
`child_copy` lost transient `ERROR_PARTIAL_COPY` rounds. It reproduced
consistently from an MSYS parent, passed under `strace` (timing), and
passed with the stock DLL (its copy path never observed a failed round).
The chunked per-chunk retries in `child_copy` fix it: with the fixed DLL,
`tools/git-priv` bash runs unconfined from an MSYS parent (`bash --version`,
rc=0), and the gate's outside-sandbox checks are green again.

### Next debugging steps (for whoever picks up the root cause) — DONE, see the resolution section above; kept for context

1. Instrument `frok::parent` / `child_copy` in msys-2.0.dll (or use a debug
   build) to log: child handle state at copy time, whether the child has
   passed DllMain, and `NtReadProcessMemory` partial-copy return detail — the
   question is *what mutates the cygheap region while it's being copied*.
2. ProcMon capture during a n=100 round filtered on the child PID: `Process
   Start`, `Thread Create`, image loads — identify who loads/patches memory in
   the child between `CreateProcess` and its DllMain completion.
3. Toggle mxc ProcessContainer knobs one at a time on the reproducer
   (`REPRO_DENIALS=1` audit mode, UI isolation on/off, captureDenials ETL) to
   find which container feature flips the behavior.
4. A/B the DLL: deploy stock (pre-patch) `msys-2.0.dll` into the container
   path — if it fails identically, the two bugs are confirmed independent
   (evidence in note 2 already says it will).

## Reproduction cases to try

> **Verified reproducer available:** `tools/repro-fork/` (self-contained —
> needs only a microsoft/mxc checkout at the pinned ref, no agent harness). It
> replays these signatures in-container deterministically (5/5 rounds at
> n=500, 4/10 at n=5) with a clean unconfined control. See its `README.md` for
> the build/run commands and measured rates. The cases below remain as
> historical context for how the failures were first observed.

Ordered from most to least promising. Run each **inside an agent session with
`shellPath` pointing at Git Bash under the MXC sandbox** (the observed
context), and once again in a plain `cmd.exe`/Git-Bash terminal as the control.

### Case A — fork load loop (stress the copy path)

```bash
for i in $(seq 1 2000); do echo "x" | cat > /dev/null; done
```

Each iteration forks a pipeline. Variants: raise to 10 000; run three such
loops concurrently (`… &` ×3) to force simultaneous cygheap copies.
**Pass criterion**: any `child_copy` / `dofork` / `Resource temporarily
unavailable` line, or non-zero exit.

### Case B — replay this session's exact hard-fail commands

```bash
# case 2: script-file execution right after a heavy build
cd internal/dap && python -c "print('x')" && rm -f /nonexistent && echo OK
# (substitute a small python file executed as `python file.py` as in the original)

# case 6: heredoc + formatter + tests in one call — verify content survived
cat >> /tmp/probe.txt <<'EOF'
line-one
EOF
gofmt -l . && go test ./internal/dap/

# cases 4/5: two-stage pipelines
grep -o 'some-regex' <file> | head -3; grep -o 'another' <file> | head -3
```

Repeat each 50× (`for i in $(seq 50); do …; done`) — the observed rate was
~1% per eligible command.

### Case C — interleave `go test` grandchild churn with forks

This matches the observed correlation (hard fails followed test runs):

```bash
for i in $(seq 1 20); do
  go test ./internal/dap/          # spawns mock-adapter grandchildren
  echo "phase $i" | cat            # fork immediately after
done
```

The mock-adapter tests re-exec the test binary (mock-adapter env flag set),
so each `go test` produces extra process trees — a good proxy for the exact
workload in this session.

### Case D — interleave CLI launches (matches case 8)

```bash
for i in $(seq 1 30); do <cli> --version > /dev/null; echo "tick $i" | cat; done
```

The CLI tool spawns its own children (and MCP servers when configured),
stressing nested process creation under the sandbox.

### Case E — environmental A/B isolations (run after A–D reproduce)

1. **Sandbox on/off**: same loop in a plain Git-Bash window (control) vs an
   agent session with escalation vs without. If only the sandboxed runs fail →
   hypothesis 1.
2. **Antivirus**: temporarily exclude `C:\Program Files\Git\**` and the
   workspace from real-time scanning; re-run A–D. If failures vanish →
   hypothesis 2.
3. **Fresh parent vs long-lived**: run Case A in the first minute of a session
   vs after 1 h / several hundred commands (keep one agent session open). Tests
   hypothesis 3.
4. **Git for Windows version**: `git --version` + `sha256sum
   /usr/bin/msys-2.0.dll`; upgrade/downgrade and re-run A. Tests hypothesis 4.

## Data to collect on a reproduction

- The verbatim stderr lines (they identify PID, cygheap region, Win32 error,
  child exit code) and the tool call's exit code (`-1073741502` = 0xC0000142).
- `bash --version; uname -a; git --version` and the hash of
  `C:\Program Files\Git\usr\bin\msys-2.0.dll`.
- RAM and handle counts of the parent `bash.exe` at failure time
  (Sysinternals Process Explorer: `handle count`, `private bytes`).
- **Process Monitor** capture filtered on `bash.exe` with `Process Start`,
  `Thread Create`, and `Operation Not Supported`/`FAST IO` failures during a
  Case A/B loop — this will show who is touching the child during DLL init
  (AV vs sandbox component vs nothing).
- Windows Event Log → *Windows Logs → Application* errors for `bash.exe` and
  any WER (`C:\ProgramData\Microsoft\Windows\WER\`) dumps referencing
  `0xC0000142`.
- Whether the harness logged anything for the same instant (its agent exit /
  crash-output logs).

## Workarounds in the meantime

- **Retry the identical command**: every hard failure in this session cleared
  on immediate retry; the harness/editor can do this automatically on exit
  code `-1073741502` (0xC0000142) with the `child_copy`/`dofork` marker.
- **Set the session's `shellPath` to a native Win32 shell** (per the
  harness's own `mxc#1061` guidance, e.g. `brush` via `cargo install brush`)
  so no MSYS fork happens inside the container at all — this removes the
  failing mechanism entirely and is the recommended fix until the root cause
  is found.
- Avoid multi-stage pipelines/heredocs for critical writes in a session that
  must not be retried (case 6 showed heredoc content can be lost).
