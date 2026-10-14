/* Driver for abi_mix_a.c / abi_mix_b.c (see abi_mix.h). */
int run_a(void);
int run_b(void);

int main(void)
{
  int bad = run_a();
  bad |= run_b();
  return bad;
}
