extern void link_error1(void);
extern void link_error2(void);
extern float _Complex conjf(float _Complex);

void test1(void)
{
  /* Non-builtin path */
  if (conjf(1.0F + 2.0iF) != 1.0F - 2.0iF)
    link_error1();
}

void test2(void)
{
  /* Builtin path */
  if (__builtin_conjf(1.0F + 2.0iF) != 1.0F - 2.0iF)
    link_error2();
}

int main(void)
{
  return 0;
}
