/* Hex float literals: leading zero digits carry no significance and must not
 * use up the 64-bit mantissa budget; digits beyond it only round. */
int printf(const char *, ...);
typedef union { double d; unsigned u[2]; } D;
static void show(const char *n, double d)
{
  D x;
  x.d = d;
  printf("%s %08x %08x\n", n, x.u[1], x.u[0]);
}
int main(void)
{
  show("a", 0x0.0000000000000001p0);
  show("b", 0x0.00000000000000000001p+80);
  show("c", 0x1.921fb54442d18469898cc51701b8p+1);
  show("d", 0x00000000000000000001.8p0);
  show("e", 0x0.8p0);
  /* 2^70 + 1: the dropped integer digits still scale the value */
  show("f", 0x400000000000000001p0);
  /* tie at bit 53 plus a nonzero dropped digit rounds up */
  show("g", 0x1.00000000000008000000001p0);
  show("h", 0x1.00000000000008p0);
  return 0;
}
