/* The documented volatile-local miscompile reproducer: a volatile local's
 * store and both of its reads must survive as real memory accesses at -O2.
 * No pass may fold a read to the initialiser constant, and the register
 * allocator must keep V0 memory-resident (no `R<n>(V0)` promotion). */
int ext(int);
int local_read_survives(int c)
{
  volatile int x = 5;
  if (c)
    return ext(x);
  return x;
}
