/* Test copy propagation optimization
 * This is the Move() function from the optimization plan
 */
#include <stdio.h>

int Move(int *source, int *dest) {
    int i = 0, j = 0;
    while (j < 4 && dest[j] == 0)
        j++;
    dest[j - 1] = source[i];
    return dest[j - 1];
}

int test_copy_chain() {
    /* Pattern: TMP <- VAR; VAR <- TMP + 1
     * Should optimize to: VAR <- VAR + 1
     */
    int count = 0;
    for (int i = 0; i < 5; i++) {
        count++;  /* Copy propagation should eliminate temp */
    }
    return count;
}

int main() {
    int source[4] = {10, 20, 30, 40};
    int dest[4] = {0, 0, 5, 0};  /* j will stop at index 2 */

    int result = Move(source, dest);
    printf("Move result: %d\n", result);
    printf("dest[1] = %d\n", dest[1]);

    printf("test_copy_chain: %d\n", test_copy_chain());
    return 0;
}
