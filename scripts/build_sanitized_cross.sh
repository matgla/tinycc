#!/bin/sh
# build_sanitized_cross.sh -- build an instrumented armv8m-tcc OUT OF TREE.
#
#   scripts/build_sanitized_cross.sh DEST [VARIANT]
#
# Copies the compiler sources (not tests/, not build products) into DEST and
# builds DEST/armv8m-tcc there, so the tree's own armv8m-tcc -- and everyone
# running tests against it -- never sees reconfigured flags or stale,
# differently-instrumented objects.  Run it again to refresh DEST after a
# source change (rsync keeps it incremental).
#
# VARIANT:
#   asan      ASan + UBSan, -O1 -g                         (default)
#   lowmem    asan + -DCONFIG_TCC_LOW_MEM  (the device's small tables/arenas)
#   o0only    asan + -DCONFIG_TCC_O0_ONLY  (Pico 2 compiler; implies LOW_MEM)
#   valgrind  no sanitizer, -O2 -g          (the normal build, with symbols)
#
# The resulting compiler is used with -B<tree> so it finds the tree's include/
# (scripts/asan_sweep.py --compiler DEST/armv8m-tcc does that).
set -eu

[ $# -ge 1 ] || { sed -n '2,20p' "$0"; exit 2; }
DEST=$1
VARIANT=${2:-asan}
TOP=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-8}
# gcc's UBSan runtime (libubsan) is often not installed even where libasan is;
# clang links its sanitizer runtimes statically.  Override with SAN_CC=gcc.
SAN_CC=${SAN_CC:-$(command -v clang >/dev/null 2>&1 && echo clang || echo gcc)}

case $VARIANT in
  asan)     CONF="--enable-asan --enable-ubsan" ; XCF="-O1 -g" ;;
  lowmem)   CONF="--enable-asan --enable-ubsan" ; XCF="-O1 -g -DCONFIG_TCC_LOW_MEM" ;;
  o0only)   CONF="--enable-asan --enable-ubsan" ; XCF="-O1 -g -DCONFIG_TCC_O0_ONLY" ;;
  valgrind) CONF="" ;                             XCF="-O2 -g" ; SAN_CC=gcc ;;
  *) echo "unknown variant: $VARIANT" >&2; exit 2 ;;
esac

mkdir -p "$DEST"
rsync -a --delete \
  --exclude=/.git --exclude=/tests/ --exclude=/.venv/ \
  --exclude=/armv8m-source/ --exclude='/armv8m-*.o' --exclude='/armv8m-*.a' \
  --exclude=/armv8m-tcc --exclude=/lib/fp/build/ --exclude='*.o' \
  --exclude=/config.mak --exclude=/config.h --exclude=/config.texi \
  --exclude='/build-*.stamp' \
  "$TOP/" "$DEST/"

cd "$DEST"
# Reconfigure only when the flags change: config.mak is a dependency of every
# object, so rewriting it forces a full rebuild.
want="$SAN_CC | $CONF | $XCF"
if [ ! -f config.mak ] || [ "$(cat .sanitize-variant 2>/dev/null)" != "$want" ]; then
  # Unity off: one TU per source keeps sanitizer frames pointing at real files.
  # shellcheck disable=SC2086
  ./configure --cc="$SAN_CC" $CONF --disable-unity --extra-cflags="-Wall $XCF -Wno-unused-result" >/dev/null
  echo "$want" > .sanitize-variant
  rm -rf armv8m-source armv8m-*.o
fi
rm -f armv8m-tcc
make -j"$JOBS" armv8m-tcc >build.log 2>&1 || { tail -30 build.log >&2; exit 1; }
echo "$DEST/armv8m-tcc ($VARIANT)"
