/* `(x<5)&&(x>5)` is a tautological contradiction, but x is volatile: both
 * comparisons must still read x from memory.  VRP / branch-fold / cmp-expr
 * folding must NOT collapse the two reads to a constant. */
void g(void);
int local_cmp_pair(int y)
{
  volatile int x = y;
  if ((x < 5) && (x > 5))
    g();
  return 0;
}
