/* Minimal reproducer for TBH offset error */
typedef unsigned int uint32_t;

int irop_btype_to_vt_btype(int irop_btype)
{
  switch (irop_btype)
  {
  case 1:
    return 10;
  case 2:
    return 20;
  case 3:
    return 30;
  case 4:
    return 40;
  case 5:
    return 50;
  case 6:
    return 60;
  case 7:
    return 70;
  default:
    return 0;
  }
}

int main(void)
{
  volatile int x = 3;
  return irop_btype_to_vt_btype(x);
}
