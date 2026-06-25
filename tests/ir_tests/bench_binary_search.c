#include <stdio.h>

int bench_binary_search(void)
{
    int data[128];
    int checksum = 0;
    int iterations = 2000;

    for (int i = 0; i < 128; i++) {
        data[i] = i * 5 + 11;
    }

    for (int n = 0; n < iterations; n++) {
        int target = data[(n * 17) & 127];
        int left = 0;
        int right = 127;

        while (left <= right) {
            int mid = left + ((right - left) / 2);

            if (data[mid] == target) {
                checksum = mid + target;
                break;
            }
            if (data[mid] < target) {
                left = mid + 1;
            } else {
                right = mid - 1;
            }
        }
    }

    return checksum;
}

int main(void)
{
    printf("binary_search: %d\n", bench_binary_search());
    return 0;
}
