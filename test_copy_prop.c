int Move(int *source, int *dest)
{
  int i = 0, j = 0;
  while (j < 4 && dest[j] == 0)
    j++;
  dest[j - 1] = source[i];
  return dest[j - 1];
}
