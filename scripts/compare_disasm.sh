#!/bin/bash
# Script to compare disassemblies between TCC -O1 and GCC -O1
# Usage: ./scripts/compare_disasm.sh [test_file.c|bubble|fibonacci] [function_name]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TCC_DIR="$(dirname "$SCRIPT_DIR")"
TCC="$TCC_DIR/armv8m-tcc"

# Handle preset examples
if [ "${1:-}" = "bubble" ]; then
    TEST_FILE="/tmp/disasm_bubble_sort.c"
    FUNC_FILTER="${2:-bubble_sort}"
    cat > "$TEST_FILE" << 'EOF'
/* Bubble sort from benchmarks - tests nested loops and array access */
void bubble_sort(int *arr, int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
}
EOF
    echo "Using bubble sort example (from benchmarks)"
elif [ "${1:-}" = "fibonacci" ]; then
    TEST_FILE="/tmp/disasm_fibonacci.c"
    FUNC_FILTER="${2:-fib}"
    cat > "$TEST_FILE" << 'EOF'
/* Fibonacci from benchmarks - tests recursion */
static int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int fibonacci(int n) {
    return fib(n);
}
EOF
    echo "Using fibonacci example (from benchmarks)"
else
    # Default test file
    TEST_FILE="${1:-/tmp/disasm_test.c}"
    FUNC_FILTER="${2:-}"
fi

# Create default test file if none provided and default doesn't exist
if [ ! -f "$TEST_FILE" ]; then
    cat > "$TEST_FILE" << 'EOF'
// Test functions for disassembly comparison

int sum_array(int *p, int n) {
    int sum = 0;
    while (n-- > 0)
        sum += *p++;
    return sum;
}

int dot_product(int *a, int *b, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int fibonacci(int n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

int max(int a, int b) {
    return (a > b) ? a : b;
}

int absolute(int x) {
    return (x < 0) ? -x : x;
}
EOF
    echo "Created default test file: $TEST_FILE"
fi

# Show usage info
if [ -z "${1:-}" ]; then
    echo "Usage: $0 [test_file.c|bubble|fibonacci] [function_name]"
    echo ""
    echo "Examples:"
    echo "  $0                          # Use default test file"
    echo "  $0 mytest.c                 # Use your own C file"
    echo "  $0 mytest.c my_function     # Compare specific function"
    echo "  $0 bubble                   # Use bubble sort benchmark"
    echo "  $0 bubble bubble_sort       # Compare bubble_sort function"
    echo "  $0 fibonacci                # Use fibonacci benchmark"
    echo ""
fi

# Output files
TCC_O1="/tmp/tcc_disasm_O1.o"
GCC_O1="/tmp/gcc_disasm_O1.o"
TCC_ASM="/tmp/tcc_disasm.s"
GCC_ASM="/tmp/gcc_disasm.s"
TCC_DUMP="/tmp/tcc_disasm.dump"
GCC_DUMP="/tmp/gcc_disasm.dump"

echo "=== Compiling $TEST_FILE ==="
echo ""

# Compile to object files
"$TCC" -O1 -c "$TEST_FILE" -o "$TCC_O1" 2>&1 || echo "TCC compilation failed"
arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -O1 -c "$TEST_FILE" -o "$GCC_O1" 2>&1 || echo "GCC compilation failed"

# Also compile to assembly source for easier reading
"$TCC" -O1 -S "$TEST_FILE" -o "$TCC_ASM" 2>&1 || true
arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -O1 -S "$TEST_FILE" -o "$GCC_ASM" 2>&1 || true

# Generate disassembly
arm-none-eabi-objdump -d "$TCC_O1" > "$TCC_DUMP" 2>&1
arm-none-eabi-objdump -d "$GCC_O1" > "$GCC_DUMP" 2>&1

# Get list of functions
TCC_FUNCS=$(arm-none-eabi-nm "$TCC_O1" 2>/dev/null | grep ' T ' | awk '{print $3}' | sort || true)
GCC_FUNCS=$(arm-none-eabi-nm "$GCC_O1" 2>/dev/null | grep ' T ' | awk '{print $3}' | sort || true)

echo "Available functions in TCC output:"
echo "$TCC_FUNCS" | sed 's/^/  /' || echo "  (none)"
echo ""
echo "Available functions in GCC output:"
echo "$GCC_FUNCS" | sed 's/^/  /' || echo "  (none)"
echo ""

# Function to extract a single function's disassembly
extract_func() {
    local dump_file="$1"
    local func_name="$2"
    
    awk -v func="$func_name" '
        /^[0-9a-f]+ <.*>:$/ {
            in_func = 0
            if (match($0, "<" func ">:")) {
                in_func = 1
            }
        }
        in_func { print }
        in_func && /^$/ { in_func = 0 }
    ' "$dump_file"
}

# Function to count instructions in disassembly
count_insts() {
    local dump_file="$1"
    local func_name="$2"
    
    extract_func "$dump_file" "$func_name" | grep -E '^\s+[0-9a-f]+:' | wc -l
}

# Compare specific function or all functions
if [ -n "$FUNC_FILTER" ]; then
    FUNCS_TO_COMPARE="$FUNC_FILTER"
else
    # Get common functions
    FUNCS_TO_COMPARE=$(echo -e "$TCC_FUNCS\n$GCC_FUNCS" | sort | uniq -d | grep -v '^$' || true)
fi

if [ -z "$FUNCS_TO_COMPARE" ]; then
    echo "No functions to compare!"
    exit 1
fi

for func in $FUNCS_TO_COMPARE; do
    echo "========================================"
    echo "  Function: $func"
    echo "========================================"
    echo ""
    
    # Count instructions
    tcc_count=$(count_insts "$TCC_DUMP" "$func" || echo 0)
    gcc_count=$(count_insts "$GCC_DUMP" "$func" || echo 0)
    
    printf "  TCC -O1:  %3d instructions\n" "$tcc_count"
    printf "  GCC -O1:  %3d instructions\n" "$gcc_count"
    
    if [ "$gcc_count" -gt 0 ]; then
        ratio=$(echo "scale=2; $tcc_count / $gcc_count" | bc 2>/dev/null || echo "N/A")
        printf "  Ratio:    %s (TCC/GCC)\n" "$ratio"
    fi
    echo ""
    
    # Show disassembly side by side if terminal is wide enough
    tcc_func_file="/tmp/tcc_func_$func.txt"
    gcc_func_file="/tmp/gcc_func_$func.txt"
    
    extract_func "$TCC_DUMP" "$func" > "$tcc_func_file"
    extract_func "$GCC_DUMP" "$func" > "$gcc_func_file"
    
    # Check if we have both disassemblies
    if [ ! -s "$tcc_func_file" ] && [ ! -s "$gcc_func_file" ]; then
        echo "  (function not found in either output)"
        continue
    fi
    
    # Header for side-by-side
    printf "  %-44s | %s\n" "TCC -O1" "GCC -O1"
    printf "  %-44s-+-%-44s\n" "--------------------------------------------" "--------------------------------------------"
    
    # Simple side-by-side using paste
    if command -v paste >/dev/null 2>&1; then
        # Pad shorter file with empty lines
        tcc_lines=$(wc -l < "$tcc_func_file" | tr -d ' ')
        gcc_lines=$(wc -l < "$gcc_func_file" | tr -d ' ')
        max_lines=$(( tcc_lines > gcc_lines ? tcc_lines : gcc_lines ))
        
        # Create temp files with same line count
        awk -v max="$max_lines" 'NR<=max {print} END {for(i=NR+1;i<=max;i++) print ""}' "$tcc_func_file" > /tmp/tcc_padded.txt
        awk -v max="$max_lines" 'NR<=max {print} END {for(i=NR+1;i<=max;i++) print ""}' "$gcc_func_file" > /tmp/gcc_padded.txt
        
        # Trim to reasonable width
        paste /tmp/tcc_padded.txt /tmp/gcc_padded.txt | while IFS=$'\t' read -r tcc_line gcc_line; do
            tcc_trim=$(echo "$tcc_line" | cut -c1-44)
            gcc_trim=$(echo "$gcc_line" | cut -c1-44)
            printf "  %-44s | %s\n" "$tcc_trim" "$gcc_trim"
        done
    else
        # Fallback: show sequentially
        echo "  --- TCC -O1 ---"
        cat "$tcc_func_file" | sed 's/^/    /'
        echo ""
        echo "  --- GCC -O1 ---"
        cat "$gcc_func_file" | sed 's/^/    /'
    fi
    
    echo ""
    
    # Clean up temp files
    rm -f "$tcc_func_file" "$gcc_func_file" /tmp/tcc_padded.txt /tmp/gcc_padded.txt
done

echo ""
echo "========================================"
echo "  Full assembly files available at:"
echo "========================================"
echo "  TCC: $TCC_ASM"
echo "  GCC: $GCC_ASM"
echo ""
echo "  Full disassembly available at:"
echo "  TCC: $TCC_DUMP"
echo "  GCC: $GCC_DUMP"
