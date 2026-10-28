#!/bin/bash
# Build a runtime DLL (new-msys-2.0.dll / new-cygwin1.dll) from a cygwin-style
# winsup tree, on top of the mingw-w64-cross toolchain (MSYS2).
#
#   tools/ci/build-runtime-dll.sh <tree> <dll-target> [extra-configure-flags]
#
# The top-level build system configures winsup via configure-target-winsup,
# which is skipped in some out-of-tree configurations; configure winsup
# explicitly instead, passing the same target exports the top-level rule
# would (CC/CXX carry the built-newlib include/lib paths).
set -eu

TREE="${1:?usage: build-runtime-dll.sh <tree> <dll-target> [extra-configure-flags]}"
DLL_TARGET="${2:?usage: build-runtime-dll.sh <tree> <dll-target> [extra-configure-flags]}"
EXTRA="${3:-}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

cd "$TREE"

# winsup/configure is generated, not committed; fresh checkouts need autogen
# (upstream CI does the same before configuring).
if [ ! -f winsup/configure ]; then
  (cd winsup && ./autogen.sh)
fi

mkdir -p build && cd build
../configure --build=x86_64-pc-cygwin --prefix=/usr --sysconfdir=/etc \
  --disable-dependency-tracking build_alias=x86_64-pc-cygwin
make -j"$JOBS" all-target-newlib

# The exact target exports configure-target-winsup uses (NORMAL_TARGET_EXPORTS).
R="$(pwd -P)/x86_64-pc-cygwin"
S="$(cd ../winsup && pwd -P)"
N="$(cd ../newlib && pwd -P)"
INC="-L$R/winsup/cygwin -isystem $S/cygwin/include -B$R/newlib/ -isystem $R/newlib/targ-include -isystem $N/libc/include"

mkdir -p x86_64-pc-cygwin/winsup && cd x86_64-pc-cygwin/winsup
# shellcheck disable=SC2086
CC="gcc $INC" CXX="g++ $INC" CFLAGS="-g -O2" CXXFLAGS="-g -O2" \
CONFIG_SITE=no-such-file sh ../../../winsup/configure \
  --srcdir=../../../winsup --cache-file=./config.cache \
  --with-newlib --enable-multilib --prefix=/usr --sysconfdir=/etc \
  --disable-dependency-tracking "--program-transform-name=s,y,y," \
  --disable-option-checking --with-target-subdir=x86_64-pc-cygwin \
  --build=x86_64-pc-cygwin --host=x86_64-pc-cygwin --target=x86_64-pc-cygwin \
  $EXTRA
cd ../..   # back to <tree>/build

# Generated headers are prerequisites of nearly every object but the DLL
# target does not depend on them; build them first.
make -j"$JOBS" -C x86_64-pc-cygwin/winsup/cygwin \
  child_info_magic.h shared_info_magic.h globals.h localtime.patched.c
# Both knobs are needed with modern toolchains: gnu++17 for the era of this
# code (C++20 rejects its non-bool operator== / comparison rewrites),
# -Wno-error for its remaining warnings.
make -j"$JOBS" -C x86_64-pc-cygwin/winsup/cygwin "$DLL_TARGET" \
  CXXFLAGS="${DLL_CXXFLAGS:--g -O2 -std=gnu++17 -Wno-error}"

DLL="$(find . -name "$DLL_TARGET" | head -1)"
test -n "$DLL" || { echo "DLL not found: $DLL_TARGET" >&2; exit 1; }
echo "BUILT_DLL=$DLL"
