/* An error raised while a call site is replaying an inlined body unwinds
 * through unary_funcall by longjmp, so the inline_restore_label_bindings()
 * that owns the hidden-label arrays never runs.  Under the ASan build the
 * resulting 72-byte leak report was printed on top of the diagnostic and
 * became the visible CI failure.  The body must be `static inline` (compiled
 * at the call site, not at its definition) and the call must sit inside a
 * nested block, which is where the expansion actually happens. */
static inline int helper(int a) { return undeclared_callee(a); }

int f(void)
{
  {
    {
      return helper(1);
    }
  }
}
