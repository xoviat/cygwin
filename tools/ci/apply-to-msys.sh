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

# The upstream and MSYS trees have different exception::myfault bodies.
# patch fuzz can otherwise place the new cygheap unlock call in the comment
# before myfault_altstack_handler.  Normalize the two exact call sites and
# remove any fuzz-misplaced copies before validating the transfer.
awk '
  /init_cygheap::unlock_on_unwind \(\);/ { next }
  /^exception::myfault / { in_myfault = 1 }
  in_myfault && /PSCOPE_TABLE table =/ {
    print
    print "  init_cygheap::unlock_on_unwind ();"
    next
  }
  /^myfault_altstack_handler / { in_altstack = 1 }
  in_altstack && /if \(me.andreas\)/ {
    print
    print "      init_cygheap::unlock_on_unwind ();"
    next
  }
  { print }
' winsup/cygwin/exceptions.cc > winsup/cygwin/exceptions.cc.fixed
mv winsup/cygwin/exceptions.cc.fixed winsup/cygwin/exceptions.cc
sed -n '/^exception::myfault /,/^}/p' winsup/cygwin/exceptions.cc \
  | grep -q 'init_cygheap::unlock_on_unwind ();' \
  || { echo "cygheap unlock missing from exception::myfault" >&2; exit 1; }
sed -n '/^myfault_altstack_handler /,/^}/p' winsup/cygwin/exceptions.cc \
  | grep -q 'init_cygheap::unlock_on_unwind ();' \
  || { echo "cygheap unlock missing from altstack handler" >&2; exit 1; }

# Prove every hunk landed: the patched files must differ from HEAD exactly
# in the expected files, with no .rej/.orig litter.
if ls winsup/cygwin/**/*.rej winsup/cygwin/*.rej 2>/dev/null | grep -q .; then
  echo "rejects produced; patch did not apply cleanly" >&2
  exit 1
fi
echo "msys2-runtime patched:"
git diff --stat -- winsup | tail -12
