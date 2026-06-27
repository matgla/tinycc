/* Phase 4: conditional jumps and loop back-edges. */

int count(int n)
{
  int i = 0;
  while (i < n) i++;
  return i;
}

int if_then_else(int x)
{
  if (x > 0) return 1;
  else if (x < 0) return -1;
  return 0;
}
