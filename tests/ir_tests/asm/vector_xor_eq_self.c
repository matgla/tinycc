#ifndef N
#define N 8
#endif

typedef int V __attribute__((vector_size(N * sizeof(int))));

__attribute__((noinline))
void vector_xor_eq_self(V *p, V *q)
{
  *p = (*p ^ *q) == *p;
}
