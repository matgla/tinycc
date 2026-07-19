#!/bin/sh
# Compile lib/fp/soft natively and run the FP conformance vectors against it.
#
# This is the fast loop for working on the soft-float library: no cross
# compiler, no QEMU, no board.  The sources are pure integer C, so the host
# build exercises exactly the same algorithms the target runs.
#
# Usage: tests/fp/run_host_softfp_test.sh [max-failures-to-print]

set -e

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SOFT="$ROOT/lib/fp/soft"
OUT="${TMPDIR:-/tmp}/host_softfp_test.$$"

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O1 -g -Wall}

# fcmp/dcmp are excluded: their AEABI entry points are hand-written Thumb
# (fcmp_asm.S / dcmp_asm.S) that cannot assemble on the host, but the
# value-returning __aeabi_[fd]cmp* helpers this test calls live in the .c files.
$CC $CFLAGS \
    -I"$HERE" -I"$SOFT" -I"$ROOT/lib/fp" -I"$ROOT/include" \
    -o "$OUT" \
    "$HERE/host_softfp_test.c" \
    "$SOFT/fadd.c" "$SOFT/fmul.c" "$SOFT/fdiv.c" "$SOFT/fcmp.c" \
    "$SOFT/dadd.c" "$SOFT/dmul.c" "$SOFT/ddiv.c" "$SOFT/dcmp.c" \
    "$SOFT/conv.c" "$SOFT/dconv.c" "$SOFT/conv64.c"

"$OUT" "$@"
status=$?
rm -f "$OUT"
exit $status
