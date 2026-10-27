/* -O2 word-aligns an `if` join inside a loop.  When the code falling into
 * the join ends in a 32-bit instruction, the pad cannot replace itself by
 * widening that one, and a NOP ran on every trip through the `if` body
 * (bench_bubble_sort: one per swap).  The block's first 16-bit instruction
 * is emitted wide instead, so the join is aligned with no NOP on the path. */
int bubble(void)
{
    int arr[64];
    for (int i = 0; i < 64; i++)
        arr[i] = (63 - i) * 7 + 100;
    for (int i = 0; i < 63; i++) {
        for (int j = 0; j < 63 - i; j++) {
            if (arr[j] > arr[j + 1]) {
                int temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
    int checksum = 0;
    for (int i = 0; i < 64; i++)
        checksum += arr[i] * i;
    return checksum;
}
