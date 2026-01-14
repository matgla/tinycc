extern int printf(const char *, ...);

void cleanup_double(double *f)
{
  printf("cleanup: %f\n", *f);
}

int main()
{
  {
    double __attribute__((__cleanup__(cleanup_double))) f = 2.6;
  }
  printf("done\n");
  return 0;
}
