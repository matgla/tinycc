/* 64-bit switch-merge without a spill storm.
 *
 * Two regalloc fixes under test (ir/regalloc.c):
 *  - ra_coalesce_graph no longer bails on SWITCH_TABLE functions (the CFG
 *    models every dispatch edge), so the per-case pair temps coalesce with
 *    the phi destination and each case computes straight into the merge
 *    pair;
 *  - the back-edge interval extension skips single-def TEMPs defined AT the
 *    jump target (a switch dispatching to linearly-earlier case blocks used
 *    to balloon every case temp to the dispatch: 8 pairs live at once,
 *    per-case spill slots + str/str/ldr/ldr round-trips, ashrdi-1
 *    constant_shift's 488-byte frame vs GCC's zero).
 *
 * The test pins: no stack traffic in the case bodies at all. */

unsigned long long sw8(unsigned long long x, int n)
{
  unsigned long long r = 0;
  switch (n)
  {
  case 1:
    r = x >> 1;
    break;
  case 2:
    r = x >> 2;
    break;
  case 3:
    r = x >> 3;
    break;
  case 4:
    r = x >> 4;
    break;
  case 5:
    r = x >> 5;
    break;
  case 6:
    r = x >> 6;
    break;
  case 7:
    r = x >> 7;
    break;
  case 8:
    r = x >> 8;
    break;
  }
  return r;
}
