/* Frame objects with disjoint lifetimes share bytes (frame.c, frame_colour).
 * Every shape here must keep the objects that are live at the same time
 * apart: values carried around a loop, addresses that escape, one-past-end
 * walks, a struct result reached through an array member while another
 * struct call runs, backward gotos, switches in loops, setjmp. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

struct blk
{
  int v[12];
};
struct name
{
  char s[16];
};

static int *kept;

__attribute__((noinline)) static struct blk mk(int k)
{
  struct blk b;
  for (int i = 0; i < 12; i++)
    b.v[i] = k * 100 + i;
  return b;
}

__attribute__((noinline)) static struct name nm(const char *s)
{
  struct name n;
  memset(&n, 0, sizeof n);
  strncpy(n.s, s, sizeof n.s - 1);
  return n;
}

__attribute__((noinline)) static int sum_blk(struct blk b)
{
  int s = 0;
  for (int i = 0; i < 12; i++)
    s += b.v[i];
  return s;
}

__attribute__((noinline)) static int sum_range(const int *p, const int *end)
{
  int s = 0;
  while (p != end)
    s += *p++;
  return s;
}

__attribute__((noinline)) static void keep(int *p)
{
  kept = p;
}

__attribute__((noinline)) static int two(const char *a, int b)
{
  return (int)strlen(a) * 1000 + b;
}

/* Sequential struct results: later ones may take earlier ones' bytes. */
__attribute__((noinline)) static int sequential(int k)
{
  struct blk a = mk(k);
  int s = a.v[0] + a.v[11];
  struct blk b = mk(k + 1);
  s += sum_blk(b);
  struct blk c = mk(k + 2);
  s += c.v[5];
  struct blk d = mk(k + 3);
  return s + sum_range(d.v, d.v + 12);
}

/* `acc` is read at the top of each iteration and written at the bottom, so
 * it is live around the whole loop; the per-iteration temporaries must not
 * land on it. */
__attribute__((noinline)) static int loop_carried(int n)
{
  struct blk acc = mk(0);
  for (int i = 0; i < n; i++)
  {
    int top = acc.v[i % 12];
    struct blk t = mk(i);
    struct blk u = mk(i + 7);
    acc.v[i % 12] = top + t.v[3] + u.v[4];
  }
  struct blk after = mk(n);
  return sum_blk(acc) + after.v[2];
}

/* An escaped address keeps its object for the rest of the function. */
__attribute__((noinline)) static int escaped(int k)
{
  int arr[8];
  for (int i = 0; i < 8; i++)
    arr[i] = k + i;
  keep(arr);
  struct blk x = mk(k);
  struct blk y = mk(k + 1);
  return kept[3] + kept[7] + x.v[1] + y.v[2];
}

/* A struct result's array reached through a pointer-returning call, while
 * another struct-returning call runs in the same expression. */
__attribute__((noinline)) static int temp_through_call(void)
{
  return two(strchr(nm("hello, world").s, 'w'), mk(4).v[3]);
}

/* A backward goto forms a loop without a loop statement. */
__attribute__((noinline)) static int goto_loop(int n)
{
  struct blk keepme = mk(9);
  int i = 0, s = 0;
again:
  {
    struct blk t = mk(i);
    s += t.v[i % 12] + keepme.v[i % 12];
  }
  if (++i < n)
    goto again;
  struct blk late = mk(n);
  return s + late.v[0] + keepme.v[11];
}

/* Cases of a switch inside a loop reach back to the loop head. */
__attribute__((noinline)) static int switch_loop(int n)
{
  struct blk carried = mk(1);
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    switch (i % 4)
    {
    case 0:
    {
      struct blk a = mk(i);
      s += a.v[1];
      break;
    }
    case 1:
    {
      struct blk b = mk(i + 1);
      carried.v[i % 12] += b.v[2];
      break;
    }
    case 2:
      s += carried.v[(i + 1) % 12];
      break;
    default:
    {
      struct blk c = mk(i + 2);
      s -= c.v[3];
      break;
    }
    }
  }
  return s + sum_blk(carried);
}

static jmp_buf env;
__attribute__((noinline)) static void jump(int v)
{
  longjmp(env, v);
}

/* setjmp returns twice: no object may take another's bytes. */
__attribute__((noinline)) static int with_setjmp(int k)
{
  struct blk before = mk(k);
  volatile int round = 0;
  if (setjmp(env) == 0)
  {
    struct blk first = mk(k + 1);
    round = first.v[0];
    jump(1);
  }
  struct blk second = mk(k + 2);
  return before.v[4] + round + second.v[5];
}

int main(void)
{
  printf("%d\n", sequential(3));
  printf("%d\n", loop_carried(20));
  printf("%d\n", escaped(5));
  printf("%d\n", temp_through_call());
  printf("%d\n", goto_loop(15));
  printf("%d\n", switch_loop(23));
  printf("%d\n", with_setjmp(2));
  return 0;
}
