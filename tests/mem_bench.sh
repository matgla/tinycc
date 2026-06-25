#!/bin/bash
# Heap-footprint measurement harness for the armv8m-tcc cross compiler.
# Uses massif on the host cross binary (identical tcc_malloc allocator + code
# path as the device, just 8-byte vs 4-byte pointers) so we can iterate fast
# without a device round-trip. Reports peak useful-heap for:
#   - fixed:  int main(){return 0;}  -c -nostdinc   (pure startup overhead)
#   - real:   tests2/129_scopes.c    (realistic small compile)
set -u
cd "$(dirname "$0")/.." || exit 1
TCC=./armv8m-tcc
TMP=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$TMP/empty.c"

peak() { # $1 = massif outfile
  python3 - "$1" <<'PY'
import re,sys
peak=0
for blk in open(sys.argv[1]).read().split('snapshot='):
    m=re.search(r'mem_heap_B=(\d+)',blk)
    if m: peak=max(peak,int(m.group(1)))
print(f"{peak} ({round(peak/1024,1)} KB)")
PY
}

echo "== fixed overhead: int main(){return 0;} -c -nostdinc =="
valgrind -q --tool=massif --massif-out-file="$TMP/m.fixed" --threshold=0.4 \
  "$TCC" -c -nostdinc "$TMP/empty.c" -o "$TMP/empty.o" 2>/dev/null
echo "  peak heap: $(peak "$TMP/m.fixed")"

echo "== realistic: tests2/129_scopes.c =="
valgrind -q --tool=massif --massif-out-file="$TMP/m.129" --threshold=0.4 \
  "$TCC" tests/tests2/129_scopes.c -o "$TMP/129.out" 2>/dev/null
echo "  peak heap: $(peak "$TMP/m.129")"

# keep the detailed outfiles for tree inspection
cp "$TMP/m.fixed" /tmp/massif.fixed.last
cp "$TMP/m.129" /tmp/massif.129.last
rm -rf "$TMP"
