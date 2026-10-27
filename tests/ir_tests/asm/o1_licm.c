/* -O1 hoists loop invariants, as gcc -O1 does (-ftree-loop-im).  Loop rotation
 * runs at -O1 too, and without LICM a rotated loop kept the constant
 * (const_body) and the constant table's address (table_dispatch) inside. */
typedef int (*op_t)(int);
int op_add(int x) { return x + 11; }
int op_mul(int x) { return x * 3; }
int op_xor(int x) { return x ^ 0x55AA; }

int const_body(int iterations)
{
  int r = 0;

  for (int n = 0; n < iterations; n++)
  {
    int i = 42; /* Fixed value for deterministic result */

    r = 1234;
    if (i & 1)
    {
      r += i * 3;
    }
    else if (i % 3 == 0)
    {
      r -= i;
    }
    else
    {
      r ^= i;
    }

    if (r > 1000000)
    {
      r = r >> 3;
    }
    else if (r < -1000000)
    {
      r = -r;
    }
  }

  return r;
}

int table_dispatch(int n)
{
    op_t ops[4] = {op_add, op_mul, op_xor, op_add};
    int v = 7;
    for (int k = 0; k < n; k++)
        v = ops[k & 3](v);
    return v;
}
