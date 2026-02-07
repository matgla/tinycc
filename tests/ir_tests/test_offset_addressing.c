/* Test for LDR/STR with offset addressing optimization
 * 
 * This test demonstrates array access patterns that should use
 * ARM's offset addressing mode: ldr rd, [base, index, LSL #2]
 * 
 * Expected optimization: Array accesses should use single instruction
 * instead of separate offset calculation + address computation + load.
 */

#include <stdio.h>

#define ARRAY_SIZE 10

int arr[ARRAY_SIZE] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

/* Test 1: Simple array load with index */
int load_array_element(int index)
{
    return arr[index];
}

/* Test 2: Simple array store with index */
void store_array_element(int index, int value)
{
    arr[index] = value;
}

/* Test 3: Array access in a loop (sequential) */
int sum_array(void)
{
    int sum = 0;
    for (int i = 0; i < ARRAY_SIZE; i++) {
        sum += arr[i];
    }
    return sum;
}

/* Test 4: Two consecutive array accesses */
int swap_adjacent(int index)
{
    int temp = arr[index];
    arr[index] = arr[index + 1];
    arr[index + 1] = temp;
    return temp;
}

/* Test 5: Multiple array accesses with same base */
int compute_with_array(int a, int b)
{
    int x = arr[a];
    int y = arr[b];
    return x * y + arr[a + b];
}

int main(void)
{
    int result = 0;
    
    /* Test load */
    result += load_array_element(5);
    
    /* Test store */
    store_array_element(0, 100);
    result += arr[0];
    
    /* Test loop sum */
    result += sum_array();
    
    /* Test swap */
    result += swap_adjacent(3);
    
    /* Test multiple accesses */
    result += compute_with_array(2, 3);
    
    printf("Result: %d\n", result);
    
    /* Expected result calculation:
     * load_array_element(5) = 5
     * arr[0] after store = 100
     * sum_array() = 100+1+2+3+4+5+6+7+8+9 = 145
     * swap_adjacent(3): arr[3]=3, arr[4]=4, after swap returns 3
     * compute_with_array(2,3): arr[2]=2, arr[3]=4 (swapped), arr[5]=5, result = 2*4+5 = 13
     * Total: 5 + 100 + 145 + 3 + 13 = 266
     */
    
    return result == 266 ? 0 : 1;
}
