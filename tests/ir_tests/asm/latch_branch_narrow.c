/* An alignment pad right after a loop's back-edge branch runs once, when the
 * loop exits; widening that branch instead costs on every trip it is taken.
 * bubble's inner latch must stay a 16-bit branch. */
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
