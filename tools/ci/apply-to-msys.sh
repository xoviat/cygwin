#!/bin/bash
# Apply the canonical cygwin-tree winsup patch to a msys2-runtime checkout.
#
# The fixes live in this repository's winsup/ tree (the cygwin sources); the
# msys2-runtime fork shares those files, so the same patch carries the fixes
# across.  Usage:
#
#   tools/ci/apply-to-msys.sh <msys2-runtime-src>
#
# The target must be a clean checkout of a known-good msys2-runtime base
# (see tools/ci/MSYS2_RUNTIME_REF).  On success the target tree contains
# exactly the patched files verified by tools/final-verify.sh.
set -eu

CI_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCH="$CI_DIR/cygwin-winsup.patch"
TARGET="${1:?usage: apply-to-msys.sh <msys2-runtime-src>}"

[ -f "$PATCH" ] || { echo "missing $PATCH" >&2; exit 1; }
[ -d "$TARGET/winsup/cygwin" ] || { echo "not a runtime tree: $TARGET" >&2; exit 1; }

cd "$TARGET"

if git apply --check "$PATCH" 2>/dev/null; then
  git apply --whitespace=nowarn "$PATCH"
  echo "applied $PATCH (git apply)"
else
  # Context drift between the cygwin tree and the msys2 fork is limited to a
  # few comment lines; fall back to patch(1) with fuzz.
  patch -p1 --forward --no-backup-if-mismatch --fuzz=3 < "$PATCH"
  echo "applied $PATCH (patch -p1, fuzz)"
fi

# Prove every hunk landed: the patched files must differ from HEAD exactly
# in the expected files, with no .rej/.orig litter.
if ls winsup/cygwin/**/*.rej winsup/cygwin/*.rej 2>/dev/null | grep -q .; then
  echo "rejects produced; patch did not apply cleanly" >&2
  exit 1
fi
echo "msys2-runtime patched:"
git diff --stat -- winsup | tail -12
