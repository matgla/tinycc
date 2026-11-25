/* slots.c -- regular C (build it with -ccompat): the handles of slots.h.
 * Eight slots; a handle is a slot's index. */
static int open_file[8];
static int in_use[8];

int slot_open(int which)
{
  for (int h = 0; h < 8; h++)
    if (!in_use[h])
    {
      in_use[h] = 1;
      open_file[h] = which;
      return h;
    }
  return -1;
}

int slot_read(int h)
{
  return 100 + open_file[h];
}

int slot_close(int h)
{
  if (h < 0 || h >= 8 || !in_use[h])
    return -1;
  in_use[h] = 0;
  return 0;
}

int slot_live(void)
{
  int n = 0;
  for (int h = 0; h < 8; h++)
    n += in_use[h];
  return n;
}
