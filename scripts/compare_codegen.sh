#!/bin/bash
# Script to compare code generation between TCC -O0, TCC -O1, and GCC -O1
# Usage: ./scripts/compare_codegen.sh [test_file.c]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TCC_DIR="$(dirname "$SCRIPT_DIR")"
TCC="$TCC_DIR/armv8m-tcc"

# Default test file
TEST_FILE="${1:-/tmp/compare_test.c}"

# Create default test file if none provided and default doesn't exist
if [ ! -f "$TEST_FILE" ]; then
    cat > "$TEST_FILE" << 'EOF'
// Test functions for code size comparison
void bubble_sort(int *arr, int n) {
    for (int i = 0; i < n-1; i++) {
        for (int j = 0; j < n-i-1; j++) {
            if (arr[j] > arr[j+1]) {
                int tmp = arr[j];
                arr[j] = arr[j+1];
                arr[j+1] = tmp;
            }
        }
    }
}

// int dot_product(int *a, int *b, int n) {
//     int sum = 0;
//     for (int i = 0; i < n; i++) {
//         sum += a[i] * b[i];
//     }
//     return sum;
// }
//
// void copy_sum(int *dst, int *src1, int *src2, int n) {
//     for (int i = 0; i < n; i++) {
//         *dst++ = *src1++ + *src2++;
//     }
// }
//
// int sum_array(int *p, int n) {
//     int sum = 0;
//     while (n-- > 0)
//         sum += *p++;
//     return sum;
// }
//
// int load_element(int *arr, int idx) {
//     return arr[idx];
// }
EOF
    echo "Created default test file: $TEST_FILE"
fi

# Output files
TCC_O0="/tmp/tcc_O0.o"
TCC_O1="/tmp/tcc_O1.o"
GCC_O1="/tmp/gcc_O1.o"

# Compile
echo "Compiling $TEST_FILE..."
"$TCC" -O0 -c "$TEST_FILE" -o "$TCC_O0"
"$TCC" -O1 -c "$TEST_FILE" -o "$TCC_O1"
arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -O1 -c "$TEST_FILE" -o "$GCC_O1"

echo ""
echo "=== Total Code Size Comparison ==="
echo "+-----------+-------+-------+-------+"
echo "| Compiler  | text  | data  |  bss  |"
echo "+-----------+-------+-------+-------+"
printf "| TCC -O0   | %5d | %5d | %5d |\n" $(arm-none-eabi-size "$TCC_O0" | tail -1 | awk '{print $1, $2, $3}')
printf "| TCC -O1   | %5d | %5d | %5d |\n" $(arm-none-eabi-size "$TCC_O1" | tail -1 | awk '{print $1, $2, $3}')
printf "| GCC -O1   | %5d | %5d | %5d |\n" $(arm-none-eabi-size "$GCC_O1" | tail -1 | awk '{print $1, $2, $3}')
echo "+-----------+-------+-------+-------+"

# Calculate ratios
TCC_O0_SIZE=$(arm-none-eabi-size "$TCC_O0" | tail -1 | awk '{print $1}')
TCC_O1_SIZE=$(arm-none-eabi-size "$TCC_O1" | tail -1 | awk '{print $1}')
GCC_O1_SIZE=$(arm-none-eabi-size "$GCC_O1" | tail -1 | awk '{print $1}')

echo ""
echo "Ratios:"
echo "  TCC -O1 / TCC -O0 = $(echo "scale=2; $TCC_O1_SIZE / $TCC_O0_SIZE" | bc)x ($(echo "scale=0; (1 - $TCC_O1_SIZE / $TCC_O0_SIZE) * 100" | bc)% reduction)"
echo "  TCC -O1 / GCC -O1 = $(echo "scale=2; $TCC_O1_SIZE / $GCC_O1_SIZE" | bc)x"

echo ""
echo "=== Per-Function Size Comparison ==="
echo ""

# Get function names
FUNCS=$(arm-none-eabi-nm "$TCC_O0" | grep ' T ' | awk '{print $3}' | sort)

printf "%-20s | %8s | %8s | %8s | %s\n" "Function" "TCC -O0" "TCC -O1" "GCC -O1" "TCC/GCC"
printf "%-20s-+-%8s-+-%8s-+-%8s-+-%s\n" "--------------------" "--------" "--------" "--------" "-------"

for func in $FUNCS; do
    tcc_o0=$(arm-none-eabi-nm -S "$TCC_O0" | grep " T $func\$" | awk '{print $2}' | xargs -I{} printf "%d" 0x{} 2>/dev/null || echo 0)
    tcc_o1=$(arm-none-eabi-nm -S "$TCC_O1" | grep " T $func\$" | awk '{print $2}' | xargs -I{} printf "%d" 0x{} 2>/dev/null || echo 0)
    gcc_o1=$(arm-none-eabi-nm -S "$GCC_O1" | grep " T $func\$" | awk '{print $2}' | xargs -I{} printf "%d" 0x{} 2>/dev/null || echo 0)

    if [ "$gcc_o1" -gt 0 ]; then
        ratio=$(echo "scale=2; $tcc_o1 / $gcc_o1" | bc)
    else
        ratio="N/A"
    fi

    printf "%-20s | %8d | %8d | %8d | %sx\n" "$func" "$tcc_o0" "$tcc_o1" "$gcc_o1" "$ratio"
done

echo ""
echo "=== Disassembly (optional) ==="
echo "To see disassembly, run:"
echo "  arm-none-eabi-objdump -d $TCC_O1 | less"
echo "  arm-none-eabi-objdump -d $GCC_O1 | less"
