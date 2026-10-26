/* ssa:loop_header_dup rewrites a loop's back-edge into a bottom test only where
 * that pays on the M33 (measured on the RP2350 board): not for a loop that
 * calls (bench_indirect_calls +8%), a straight-line body (sha_transform's
 * rounds +3.6%) or a loop with a second exit (bench_binary_search's search);
 * still for a body that branches onto the back-edge (mibench_dijkstra's
 * relaxation loop, -7%). */
typedef int (*op_t)(int);
int op_add(int x) { return x + 11; }
int op_mul(int x) { return x * 3; }

int dispatch(int n)
{
    op_t ops[4] = {op_add, op_mul, op_add, op_mul};
    int v = 7;
    for (int k = 0; k < n; k++)
        v = ops[k & 3](v);
    return v;
}

void expand(unsigned *w)
{
    for (int i = 16; i < 80; ++i)
        w[i] = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
}

int search(const int *data, int target)
{
    int left = 0, right = 127;
    while (left <= right) {
        int mid = left + ((right - left) / 2);
        if (data[mid] == target)
            return mid;
        if (data[mid] < target)
            left = mid + 1;
        else
            right = mid - 1;
    }
    return -1;
}

int dist[100], adj[100][100];

void relax(int cur, int base)
{
    for (int node = 0; node < 100; node++) {
        int cost = adj[cur][node];
        if (cost == 9999)
            continue;
        if (dist[node] == 9999 || dist[node] > base + cost)
            dist[node] = base + cost;
    }
}
