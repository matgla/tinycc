/* A 21-word struct passed by value on the stack is block-copied into the
 * outgoing argument area with r0-r3/ip/lr as scratch.  Only registers still
 * holding a value the call setup reads are saved around that copy. */
struct S21 { int v[21]; };
extern int take4(int a, int b, int c, int d, struct S21 s, int e);
extern int takes(int a, struct S21 s, int e);
extern void getit(struct S21 *o, int k);

/* nothing live in r0-r3 across the copy: no save at all */
int fresh_local(int a)
{
  struct S21 t;
  getit(&t, a);
  return take4(a, 1, 2, 3, t, a + 1);
}

/* r0-r3 are identity arguments: all four must be saved */
int pass_through(int a, int b, int c, int d, const struct S21 *p, int e)
{
  return take4(a, b, c, d, *p, e);
}

/* only r0 (the first register argument) is live across the copy */
int split_src(const struct S21 *p, int a, int e)
{
  return takes(a, *p, e);
}
