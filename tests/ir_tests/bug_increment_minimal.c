void dummy(int x)
{
}

int test()
{
  int i;
  for (i = 0; i < 3; i++)
  {
    dummy(i);
  }
  return i;
}

int main()
{
  return test();
}
