extern int printf(const char *, ...);

void cleanup_double(double *f)
{
  unsigned char *p = (unsigned char *)f;
  printf("bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
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
