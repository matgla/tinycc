/* SCCP forwarded an anon local's StackLoc store into loads of UNRELATED
 * address-taken vars.  IRLiveInterval.original_offset for a local VAR is the
 * frontend's creation-time `loc` watermark, not a real frame slot (that only
 * exists after regalloc), so several address-taken vars can report the same
 * offset — which can also equal an unrelated anon StackLoc (here `m`'s
 * first field).  sccp_resolve_var matched `m.kind = 1/2` by that offset and
 * folded the out-params `sym`/`off` to 1 and 2.
 *
 * This is the exact shape of memref.c's memloc_of: in the self-hosted
 * device tcc it broke memloc_of's frame-offset resolution, which flipped
 * copy_source_load_fwd's decisions and produced stale bitfield reads
 * (device ir_tests 174/178/182).  Compiled directly, the shape reproduces
 * on the host cross too — `mk(1).off` returned 1 instead of 77.
 */
#include <stdio.h>

typedef struct
{
  int kind;
  void *sym;
  long long off;
  int size;
} Loc;

int g_target = 5;

/* Opaque out-param writer: fills *sym_out/*off_out, returns success. */
__attribute__((noinline)) static int resolve(int sel, void **sym_out,
                                             long long *off_out)
{
  if (sel < 0)
    return 0;
  *sym_out = sel ? (void *)&g_target : (void *)0;
  *off_out = sel ? 77 : -42;
  return 1;
}

static Loc mk(int sel)
{
  Loc m = {0, 0, 0, 0};
  m.size = 4;

  void *sym = 0;
  long long off = 0;
  if (resolve(sel, &sym, &off))
  {
    if (sym)
    {
      m.kind = 1; /* the anon-slot store SCCP mistook for sym/off's slot */
      m.sym = sym;
      m.off = off;
    }
    else
    {
      m.kind = 2;
      m.off = off;
    }
    return m;
  }
  m.kind = 3;
  return m;
}

int main(void)
{
  Loc a = mk(1);
  Loc b = mk(0);
  Loc c = mk(-1);

  printf("a=%d %d %lld\n", a.kind, a.sym == (void *)&g_target, a.off);
  printf("b=%d %d %lld\n", b.kind, b.sym == (void *)0, b.off);
  printf("c=%d\n", c.kind);

  if (a.kind == 1 && a.sym == (void *)&g_target && a.off == 77 &&
      b.kind == 2 && b.off == -42 && c.kind == 3)
    printf("OK\n");
  else
    printf("FAIL\n");
  return 0;
}
