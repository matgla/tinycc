/* A body that reads its literal pool starts word-aligned, so two identical
 * ones compile to the same machine code and fold (test_literal_body_word_aligned). */
extern void sink(unsigned);

/* An odd number of halfwords, so what follows would start at 2 mod 4. */
void pad_before(void)
{
  sink(1);
}

static __attribute__((noinline)) void lit_a(int x)
{
  sink(0x12345678u + x);
  sink(0x9abcdef0u);
}

static __attribute__((noinline)) void lit_b(int x)
{
  sink(0x12345678u + x);
  sink(0x9abcdef0u);
}

void use_both(int x)
{
  lit_a(x);
  lit_b(x + 1);
}
