/* More call-crossing values than r4-r7: the ones only passed from call to
 * call take r8-r11, the one compared with constants after every call keeps a
 * low register, so its compares are 16-bit (test_hi_callee_pref). */
extern int step(int);
extern void use(void *, void *, void *, void *, void *);

int keep_low(void *a, void *b, void *c, void *d, void *e, int n)
{
  int k = step(n);
  use(a, b, c, d, e);
  if (k == 3)
    return 1;
  k = step(k);
  use(a, b, c, d, e);
  if (k == 5)
    return 2;
  k = step(k);
  use(a, b, c, d, e);
  if (k == 7)
    return 3;
  return k == 9;
}
