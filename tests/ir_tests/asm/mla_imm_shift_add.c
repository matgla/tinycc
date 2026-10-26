/* `x * m + c` with m = 2^k, 2^k + 1 or 2^k - 1 is a shift-add and an add, with
 * no constant in a register: an MLA loaded m (and c) on every evaluation --
 * in bench_linked_list's init loop on every trip. */
struct node
{
    int value;
    struct node *next;
};
struct node nodes[100];

int a3(int x) { return x * 3 + 7; }
int a7(int x) { return x * 7 - 100; }
int a8(int x) { return x * 8 + 4000; }

void fill(int n)
{
    for (int i = 0; i < n; i++) {
        nodes[i].value = i * 3 + 7;
        nodes[i].next = (i < 99) ? &nodes[i + 1] : 0;
    }
}
