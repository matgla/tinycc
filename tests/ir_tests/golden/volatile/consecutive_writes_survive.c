/* Two consecutive writes to a volatile local are both mandated side effects;
 * dead-store / dead-overwrite elimination must not drop the first. */
int ext(int);
int consecutive_writes_survive(void)
{
  volatile int x;
  x = 1;
  x = 2;
  return ext(x);
}
