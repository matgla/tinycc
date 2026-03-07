extern int printf(const char *, ...);

/* Global complex double */
_Complex double g = 3.0 + 1.0i;

int main(void)
{
  printf("global: (%f, %f)\n", __real__ g, __imag__ g);
  return 0;
}
