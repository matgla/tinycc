/* dom-LICM hoisting of an invariant read of a local's OWN stack slot.
 *
 * `cur = queue[h++]` is a 12-byte struct copy the frontend lowers to
 * `T = &cur; __aeabi_memmove4(T, &queue[h], 12)`.  The `&cur` makes the slot
 * address-taken, which used to block LICM: the inner loop stores to globals,
 * so a store-through-a-pointer could in principle hit `cur`.  memmove neither
 * stores nor publishes the pointer, and the call sits outside the inner loop,
 * so `adj[cur.node][0]` IS invariant there and must be hoisted
 * (mibench_dijkstra: 4 instructions x 100 iterations x every outer pass).
 *
 * The three cases below pin both directions of that reasoning:
 *   ok_hoist  — the memmove shape; the address does not escape the copy.
 *   no_hoist_escaped — the same loop after `&cur` is stored to a global, which
 *                      the loop then writes through.  Hoisting the read would
 *                      use a stale `cur.node`.
 *   no_hoist_written — the loop writes the slot directly through a pointer
 *                      taken INSIDE it (the bug_struct_field_postinc shape).
 * A wrong hoist changes the checksum; it does not merely slow things down.
 */
#include <stdio.h>

#define N 8

typedef struct
{
  int node;
  int dist;
  int prev;
} item_t;

static int adj[N][N];
static int out[N];
static item_t queue[N];
static item_t *escaped;

static void setup(void)
{
  for (int r = 0; r < N; r++)
  {
    for (int c = 0; c < N; c++)
      adj[r][c] = (r * 7 + c * 3) % 11;
    queue[r].node = (r * 3) % N;
    queue[r].dist = r;
    queue[r].prev = N - r;
  }
}

/* The hoistable shape: `cur`'s address reaches only the struct copy. */
static int ok_hoist(void)
{
  int sum = 0;
  for (int h = 0; h < N; h++)
  {
    item_t cur = queue[h];
    for (int i = 0; i < N; i++)
    {
      out[i] = adj[cur.node][i] + cur.dist; /* base is loop-invariant */
      sum += out[i];
    }
  }
  return sum;
}

/* `&cur` escapes to a global the loop writes through, so `cur.node` changes
 * under the loop and the base address is NOT invariant. */
static int no_hoist_escaped(void)
{
  int sum = 0;
  for (int h = 0; h < N; h++)
  {
    item_t cur = queue[h];
    escaped = &cur;
    for (int i = 0; i < N; i++)
    {
      sum += adj[cur.node][i];
      escaped->node = (cur.node + 1) % N; /* mutates the slot */
    }
  }
  return sum;
}

/* The slot is written through a pointer taken inside the loop. */
static int no_hoist_written(void)
{
  int sum = 0;
  for (int h = 0; h < N; h++)
  {
    item_t cur = queue[h];
    item_t *p = &cur;
    for (int i = 0; i < N; i++)
    {
      sum += adj[cur.node][i];
      p->node = (p->node + 3) % N;
    }
  }
  return sum;
}

/* The loop counter IS a struct field, incremented through the field's own
 * address (`s.idx++` lowers to LEA + deref-store, which is NOT a direct slot
 * write), so the slot changes every iteration and `adj[s.idx]`'s base is not
 * invariant.  This is the shape that catches an address-matcher comparing raw
 * operand fields: `&s.idx` is STRUCT-typed, so its offset lives in u.s.aux_data
 * while its imm32 is a pool index — it never equals the plain INT32 read of
 * the same slot, and the slot then looks as if its address were never taken. */
typedef struct
{
  int idx;
  int guard;
} ctr_t;

static int no_hoist_field_counter(void)
{
  ctr_t s;
  int sum = 0;
  s.guard = 0;
  for (s.idx = 0; s.idx < N; s.idx++)
    sum += adj[s.idx][1] * 10 + s.guard;
  return sum;
}

int main(void)
{
  setup();
  printf("ok=%d esc=%d wr=%d ctr=%d\n", ok_hoist(), no_hoist_escaped(),
         no_hoist_written(), no_hoist_field_counter());
  return 0;
}
