#include <stdio.h>

/*
 * Pure-call hoisting regression test (docs/bugs.md #7, eighth defect;
 * combo fuzz seeds 52/80/187/311/333/392/460).
 *
 * `mix` is a const function with loop-invariant arguments, so
 * tcc_ir_hoist_pure_calls hoists the call into the preheader.  The loop
 * body also contains a switch.  insert_instruction_before patched
 * JUMP/JUMPIF targets while making room for the hoisted CALL+PARAMs, but
 * NOT the SWITCH_TABLE side table (ir->switch_tables), so every case and
 * default target went stale by the insertion count.  Downstream
 * reachability-based passes then deleted live FUNCPARAMVALs ("missing
 * FUNCPARAMVAL for call_id=N" compile error) or, when the IR survived to
 * codegen, the dispatch jumped into the middle of the wrong case at
 * runtime (infinite loops / wrong checksums).
 *
 * The fix renumbers switch-table targets on insertion (mirroring
 * gsym_cse_insert_before) and treats out-of-loop switch case targets as
 * external entry edges in the preheader guard.
 */

/* Const (reads no memory), but large enough that the inliner leaves the
 * call in place. */
static unsigned
mix (unsigned a, unsigned b)
{
  unsigned t = b * 2654435761u;
  unsigned r = (a ^ t) + (b >> 3);
  r = r ^ (r >> 7);
  r = r * 97u + 13u;
  r = r ^ (b << 5);
  return r;
}

int
main (void)
{
  unsigned cs = 0x12345678u;
  unsigned u = 2013416737u;
  unsigned *p = &u;
  unsigned i = 0;

  /* A while loop (not for): the rotated for-loop's increment-trampoline
   * shape keeps its body outside the loop's linear [start,end] range, where
   * the hoister (correctly) no longer looks.  The while shape matches combo
   * seed 52: linear range covers the whole body, the hoist fires, and the
   * switch table inside the loop exercises the insertion renumbering. */
  while (i < 9u)
    {
      /* Selector depends on cs so every stale-target dispatch corrupts
       * the checksum on some iteration.  5 dense cases: enough for
       * switch_can_use_jump_table (>= 4 cases, >= 50% density) to emit a
       * real SWITCH_TABLE at -O1+ instead of a compare chain. */
      switch ((cs ^ i) & 7u)
        {
        case 0:
          cs += mix (7u, 1449453030u); /* invariant args -> hoisted */
          *p = u + 13u;
          break;
        case 1:
          cs ^= u;
          break;
        case 2:
          cs = cs * 33u + i;
          break;
        case 3:
          cs += (u >> 3);
          break;
        case 4:
          cs = (cs << 5) | (cs >> 27);
          break;
        default:
          cs -= 94u;
          break;
        }
      i++;
    }

  printf ("cs=%u u=%u\n", cs, u);
  return 0;
}
