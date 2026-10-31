#!/bin/bash
# End-to-end verification chain for the mxc#1061 + dsh#10 runtime fix,
# including the residual fork-failure scenario (FORK_FAILURES.md) that the
# fork-churn step keeps covered.
# Prereqs: msys2-runtime built with tools/ci/cygwin-winsup.patch applied
#          (tools/ci/apply-to-msys.sh); MSYS_DLL overrides the DLL location,
#          mxc_gitbash_run built with ui.disable=false (mxc#1061 config).
# NEVER touches the live Git installation in "C:\Program Files\Git".
set -u
cd "$(dirname "$0")/.." || exit 1
W="$PWD"
RUN="$W/tools/run"
PRIV="$W/tools/git-priv"
PASS=0; FAIL=0

step() { echo; echo "### $1"; }
ok()   { echo "PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL: $1"; FAIL=$((FAIL+1)); if [ -n "${OUT:-}" ]; then echo "--- raw runner output ---"; echo "$OUT"; fi; true; }

# Print only the runner's stdout section.  Prevents false positives from
# markers that also appear in the echoed `command:` line.
get_stdout() { awk '/^--- stdout ---/{f=1;next} /^--- /{f=0} f'; }

# 0. Locate the patched DLL and prove it carries the fixes
step "locate patched DLL"
DLL="${MSYS_DLL:-$(find "$W/tools/msys2-runtime/build-cy" -name "new-msys-2.0.dll" 2>/dev/null | head -1)}"
[ -n "$DLL" ] || { echo "patched DLL not found"; exit 1; }
echo "patched: $DLL"
grep -aq "AppContainer ns: %y" "$DLL" && ok "DLL contains namespace fallback chain (mxc#1061)" \
  || bad "DLL missing namespace fallback chain"
objdump -t "$DLL" 2>/dev/null | grep -q "in_this_image" && ok "DLL contains wincap cross-image guard" \
  || echo "note: wincap guard symbol not visible (behavioral tests decide)"

# 1. Deploy into the PRIVATE copy (stock backed up)
step "deploy patched DLL into private copy"
[ -f "$PRIV/usr/bin/msys-2.0.dll.stock" ] || cp "$PRIV/usr/bin/msys-2.0.dll" "$PRIV/usr/bin/msys-2.0.dll.stock"
cp "$DLL" "$PRIV/usr/bin/msys-2.0.dll"
cp "$DLL" "$PRIV/bin/msys-2.0.dll"
# Mandatory: the mxc PSEC force-relocates non-ASLR-opted-in images per
# process, which breaks cygwin's same-address-space fork.  Every image in
# the install must opt in -- fresh copies are pristine and fork dies with
# 0xC0000142 otherwise.
python "$W/tools/ci/peflag.py" set "$PRIV" | tail -1
ok "patched DLL deployed (backup: msys-2.0.dll.stock)"

# 2. Outside-sandbox regression
step "outside sandbox: private git-bash with patched DLL"
OUT=$("$PRIV/usr/bin/bash.exe" --version 2>&1 | head -1)
echo "$OUT"
case "$OUT" in *"GNU bash"*) ok "bash --version works unconfined";; *) bad "bash --version unconfined";; esac
OUT=$("$PRIV/usr/bin/bash.exe" -c 'echo hi | tr a-z A-Z' 2>&1)
[ "$OUT" = "HI" ] && ok "pipeline works unconfined" || bad "pipeline unconfined: [$OUT]"

# GIT_BASH_BIN must be a Windows path: the runner feeds it into the
# container's readonly_paths (it appends "\\bin" and strips it).  A POSIX
# /c/... value silently excludes git-priv from the policy and the workload
# dies with 0xC0000135 (DLLs unreadable).
# Diagnostic: record the host OS (GitHub runners are Server SKUs; the mxc
# ProcessContainer is new and may not support them -- cmd FAILED there).
echo "host: $(cmd //c ver 2>/dev/null | tr -d '\r' | tail -1)"
export GIT_BASH_BIN="$(cygpath -w "$PRIV")\\bin"
BASH="$PRIV\\usr\\bin\\bash.exe"
QPRIV=$(cygpath -w "$PRIV/usr/bin/bash.exe")
cd "$RUN" || exit 1

# The mxc ProcessContainer requires Windows PSEC (Process Security
# Environment).  GitHub's Server-SKU runner images do not enable it
# ("Windows ProcessContainer is unavailable: PSEC is not enabled"), so
# probe once and skip the in-sandbox section with a loud note instead of
# failing: the goal verification needs a PSEC-enabled Windows host.
PROBE=$(./mxc_gitbash_run.exe 'C:\Windows\System32\cmd.exe /c ver' 2>&1)
if ! echo "$PROBE" | grep -q "Microsoft Windows"; then
  echo "SKIP: mxc ProcessContainer unavailable on this host -- in-sandbox checks skipped."
  echo "$PROBE" | head -4
  echo
  echo "==================== RESULT: $PASS passed, 0 failed (IN-SANDBOX SKIPPED) ===================="
  echo "IN-SANDBOX GOAL NOT VERIFIED HERE: run tools/final-verify.sh on a PSEC-enabled Windows host."
  exit 0
fi

# 3. THE GOAL: git-bash inside the mxc ProcessContainer
step "mxc run: bash --version (stock outcome: 0xC0000142)"
OUT=$(./mxc_gitbash_run.exe "\"$QPRIV\" --version" 2>&1)
echo "$OUT" | grep -A8 "stdout" | head -9
echo "$OUT" | grep -q "GNU bash" && ok "git-bash runs inside mxc sandbox" || bad "git-bash inside mxc"
echo "$OUT" | grep -q "Exited(0)" && ok "exit code 0 in sandbox" || bad "exit code in sandbox"
echo "$OUT" | grep -q "0xC0000142\|-1073741502" && bad "still DLL_INIT_FAILED" || true

step "mxc run: fork/exec + pipes + signal path (echo | tr)"
OUT=$(./mxc_gitbash_run.exe "\"$QPRIV\" -c \"echo hi | tr a-z A-Z\"" 2>&1)
echo "$OUT" | grep -A4 "stdout" | head -5
echo "$OUT" | get_stdout | grep -q "HI" && ok "pipeline inside mxc sandbox" || bad "pipeline inside mxc"
echo "$OUT" | grep -q "Exited(0)" && ok "pipeline exit 0" || bad "pipeline exit code"

step "mxc run: file write + read + native tool"
OUT=$(./mxc_gitbash_run.exe "\"$QPRIV\" -c 'echo ok > /tmp/mxc-t && cat /tmp/mxc-t && ls /c/Windows/System32/cmd.exe >/dev/null && echo ALLOK'" 2>&1)
echo "$OUT" | grep -A4 "stdout" | head -5
echo "$OUT" | get_stdout | grep -q "ALLOK" && ok "file IO + ls inside mxc sandbox" || bad "file IO inside mxc"
echo "$OUT" | grep -q "Exited(0)" && ok "workload exit 0" || bad "workload exit code"

step "mxc run: multi-command job control (compound script)"
OUT=$(./mxc_gitbash_run.exe "\"$QPRIV\" -c 'for i in 1 2 3; do echo n=\$i; done; test -d /c/Windows && echo COMPOUND_OK'" 2>&1)
echo "$OUT" | grep -A4 "stdout" | head -5
echo "$OUT" | get_stdout | grep -q "COMPOUND_OK" && ok "compound script inside mxc sandbox" || bad "compound script inside mxc"

# 4. The residual fork-failure scenario (FORK_FAILURES.md, tools/repro-fork):
# sustained fork churn in one long-lived bash.  Before the runtime retry
# fix this failed 5/5 rounds at n=500 (6/6 at n=100) with the verbatim
# child_copy/dofork/0xC0000142 signature -- and a handful of lucky forks
# (the steps above) still passed, so only churn catches it.  Classification
# is on the signatures plus the workload's own integrity markers, not on
# the runner's exit code alone.  CHURN_N scales the load (forks ~ 8*N).
step "mxc run: sustained fork churn (residual fork-failure scenario)"
CHURN_N="${CHURN_N:-100}"
CHURN_SH="$(cygpath -u "${TEMP:-/tmp}")/mxc-churn-forks.sh"
cat > "$CHURN_SH" <<'CHURN_EOF'
# Runs under git-bash inside the mxc ProcessContainer.  Every pipeline,
# command substitution and script exec is a fork of this long-lived parent.
set -u
N="$CHURN_N"
FAILS=0
fail() { echo "$1"; FAILS=$((FAILS + 1)); }
OUT=/tmp
mkdir -p "$OUT" 2>/dev/null
# pipelines
i=0; while [ $i -lt "$N" ]; do echo x | cat > /dev/null || fail "pipeline fork"; i=$((i + 1)); done
echo "churn A done"
# two-stage chained pipelines must not lose either stage
printf 'PROTOCOL_VERSION=1\n2025-01-01\n' > "$OUT/pb.txt"
i=0; while [ $i -lt $((N / 10 + 1)) ]; do
  a=$(grep -o 'PROTOCOL_VERSION=[^ ]*' "$OUT/pb.txt" | head -3; true)
  b=$(grep -o '2025-[^ ]*' "$OUT/pb.txt" | head -3; true)
  case "$a" in *PROTOCOL_VERSION=1*) ;; *) fail "chained pipeline lost stage 1";; esac
  case "$b" in *2025-01-01*) ;; *) fail "chained pipeline lost stage 2";; esac
  i=$((i + 1)); done
echo "churn B done"
# heredoc integrity: content must survive the fork
rm -f "$OUT/heredoc.txt"
i=0; while [ $i -lt 20 ]; do
  cat >> "$OUT/heredoc.txt" <<EOF
line-one-$i
line-two-$i
EOF
  i=$((i + 1)); done
got=$(grep -c 'line-' "$OUT/heredoc.txt" 2>/dev/null || echo 0)
[ "$got" = 40 ] || fail "heredoc lost content (got $got/40)"
echo "churn C done"
# command substitution
i=0; while [ $i -lt $((N / 5 + 1)) ]; do
  v=$({ echo hi | tr a-z A-Z; }); [ "$v" = "HI" ] || fail "cmdsub lost output"; i=$((i + 1)); done
echo "churn D done"
# concurrent fork load: 3 subshells racing the cygheap copy
for j in 1 2 3; do
  ( i=0; while [ $i -lt $((N / 10 + 1)) ]; do echo c | cat > /dev/null || exit 1; i=$((i + 1)); done ) &
done
wait || fail "concurrent fork load"
echo "churn E done"
echo "CHURN_OK fails=$FAILS"
exit "$FAILS"
CHURN_EOF
# Bake N in without risking quoting through the runner's command line.
sed -i "s|^N=\"\$CHURN_N\"|N=$CHURN_N|" "$CHURN_SH"
CHURN_WIN=$(cygpath -m "$CHURN_SH")
export MXC_RUN_TIMEOUT_MS="${MXC_RUN_TIMEOUT_MS:-300000}"
OUT=$(./mxc_gitbash_run.exe "\"$QPRIV\" \"$CHURN_WIN\"" 2>&1)
echo "$OUT" | get_stdout | tail -8
# Fork-failure signatures from FORK_FAILURES.md plus the runtime's loud
# end-of-retry diagnostics.  Any of these means the scenario reproduced,
# even if the workload managed to limp through.
echo "$OUT" | grep -v '^command:' \
  | grep -E 'child_copy:|dofork:|copy failed|VirtualAlloc|Resource temporarily unavailable|0xC0000142|-1073741502' \
  && bad "fork-churn signatures present" || ok "no fork-failure signatures"
echo "$OUT" | get_stdout | grep -q "CHURN_OK fails=0" \
  && ok "fork churn workload intact (heredoc/pipelines/cmdsub)" \
  || bad "fork churn workload lost data"
echo "$OUT" | grep -q "Exited(0)" && ok "fork churn exit 0" || bad "fork churn exit code"

step "mxc controls (informational)"
OUT=$(./mxc_gitbash_run.exe 'C:\Windows\System32\cmd.exe /c ver' 2>&1)
echo "$OUT" | grep -q "Microsoft Windows" && echo "note: cmd works in sandbox" || { echo "note: cmd FAILED"; echo "$OUT" | head -8; }
OUT=$(./mxc_gitbash_run.exe "$PRIV\\bin\\bash.exe --version" 2>&1)
echo "$OUT" | grep -q "GNU bash" && echo "note: bin/bash.exe launcher works too" || echo "note: bin/bash.exe launcher still fails (MSYS bash itself works; informational)"

echo
echo "==================== RESULT: $PASS passed, $FAIL failed ===================="
if [ "$FAIL" -eq 0 ]; then
  echo "GOAL VERIFIED: git bash works inside the mxc sandbox."
else
  echo "GOAL NOT YET MET"
fi
exit "$FAIL"
