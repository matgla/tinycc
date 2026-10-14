/* A block-scope `extern int g;` names the file-scope g: the optimizers must
 * not treat the two spellings as different objects. */
int printf(const char *, ...);
int g;
int h = 3;
__attribute__((noinline)) int t_store_store_load(int a)
{
  g = a;
  {
    extern int g;
    g = a + 5;
  }
  return g;
}
__attribute__((noinline)) int t_load_store_load(int a)
{
  g = a;
  int x = g;
  {
    extern int g;
    g = x + 7;
  }
  return g + x;
}
__attribute__((noinline)) int t_inner_first(int a)
{
  int r;
  {
    extern int g;
    g = a;
  }
  g = g + 1;
  r = g;
  {
    extern int g;
    g = r * 2;
  }
  return g;
}
__attribute__((noinline)) int t_dead_store(int a)
{
  g = a;      /* read below through the inner name, not dead */
  {
    extern int g;
    return g + 1;
  }
}
__attribute__((noinline)) int t_two_globals(int a)
{
  g = a;
  h = a + 1;
  {
    extern int g;
    extern int h;
    g = h * 2;
  }
  return g + h;
}
int main(void)
{
  printf("%d %d %d %d %d\n", t_store_store_load(10), t_load_store_load(10), t_inner_first(4), t_dead_store(6),
         t_two_globals(2));
  return 0;
}
