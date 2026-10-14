/* IVSR started the walking pointer one stride ahead whenever the address use
 * had a larger instruction index than the IV update -- but a `continue` latch
 * is laid out ahead of the body, so the update has the smaller index and still
 * runs after the use.  The loop then skipped element 0 and read one past the
 * end: the device tcc's register-order loop in ra_linear_scan did exactly that,
 * and every object it compiled differed from the host cross's. */

static const int order_a[13] = {0, 1, 2, 3, 12, 4, 5, 6, 7, 8, 9, 10, 11};
static const int order_b[13] = {0, 1, 2, 3, 4, 5, 6, 7, 12, 8, 9, 10, 11};

__attribute__((noinline)) static int pick(const int *order, unsigned long long avail, int lim, int pass)
{
  int reg = -1;
  for (int oi = 0; oi < 13; oi++)
  {
    int r = order[oi];
    if (r >= lim)
      continue;
    if (!(avail & (1ull << r)))
      continue;
    if (pass == 0 && r >= 4 && r != 12)
      continue;
    reg = r;
    break;
  }
  return reg;
}

struct item { int key; void *val; };

__attribute__((noinline)) static int find(const struct item *it, int n, int key)
{
  for (int i = 0; i < n; i++)
  {
    if (it[i].key < 0)
      continue;
    if (it[i].key == key)
      return i;
  }
  return -1;
}

int main(void)
{
  if (pick(order_a, 0x1fffull, 13, 1) != 0)
    return 1;
  if (pick(order_a, 0x1ff0ull, 13, 0) != 12)
    return 2;
  if (pick(order_a, 0x0800ull, 13, 1) != 11)
    return 3;
  if (pick(order_b, 0x0001ull, 13, 0) != 0)
    return 4;
  if (pick(order_b, 0x0000ull, 13, 1) != -1)
    return 5;
  struct item it[6] = {{7, 0}, {-1, 0}, {3, 0}, {9, 0}, {-5, 0}, {11, 0}};
  if (find(it, 6, 7) != 0)
    return 6;
  if (find(it, 6, 11) != 5)
    return 7;
  if (find(it, 6, 4) != -1)
    return 8;
  return 0;
}
