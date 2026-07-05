/* Phase 4: switch table and branch narrowing. */

int switch_small(int x)
{
  switch (x) {
    case 0: return 10;
    case 1: return 20;
    case 2: return 30;
    case 3: return 40;
    default: return 0;
  }
}
