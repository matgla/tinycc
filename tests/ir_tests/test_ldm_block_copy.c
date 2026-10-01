/* A small aggregate copied through a pointer comes out of the frontend as
 * every word loaded, then every word stored, and the codegen folds the run
 * into one LDM and one STM through the registers the loads were given.  What
 * that must not disturb: a source read again after the copy (the words are not
 * dead, so the run is not one), a destination whose address is live past the
 * copy (the LDM must not land on it), a copy whose words are not consecutive,
 * and a copy of an object onto itself.  Offsets on either side, both sides, or
 * neither all have to address the same bytes. */
#include <stdint.h>
#include <stdio.h>

struct w3
{
  uint32_t a[3];
};

struct w4
{
  uint32_t a[4];
};

struct w6
{
  uint32_t a[6];
};

struct nested
{
  uint32_t head;
  struct w4 mid;
  uint32_t tail;
};

static uint32_t sum3(const struct w3 *p) { return p->a[0] * 1 + p->a[1] * 3 + p->a[2] * 7; }
static uint32_t sum4(const struct w4 *p) { return p->a[0] * 1 + p->a[1] * 3 + p->a[2] * 7 + p->a[3] * 13; }

__attribute__((noinline)) static void fill3(struct w3 *p, uint32_t s)
{
  for (int i = 0; i < 3; i++)
    p->a[i] = s + (uint32_t)i;
}

__attribute__((noinline)) static void fill4(struct w4 *p, uint32_t s)
{
  for (int i = 0; i < 4; i++)
    p->a[i] = s + (uint32_t)i;
}

/* pointer -> frame slot, three and four words */
__attribute__((noinline)) static uint32_t ptr_to_local3(const struct w3 *p)
{
  struct w3 b;
  b = *p;
  return sum3(&b);
}

__attribute__((noinline)) static uint32_t ptr_to_local4(const struct w4 *p)
{
  struct w4 b;
  b = *p;
  return sum4(&b);
}

/* frame slot -> pointer */
__attribute__((noinline)) static void local_to_ptr(struct w4 *d, uint32_t s)
{
  struct w4 b;
  fill4(&b, s);
  *d = b;
}

/* pointer -> pointer */
__attribute__((noinline)) static void ptr_to_ptr(struct w4 *d, const struct w4 *s) { *d = *s; }

/* a displacement on the source, on the destination, and on both */
__attribute__((noinline)) static void off_src(struct w4 *d, const struct nested *s) { *d = s->mid; }

__attribute__((noinline)) static void off_dst(struct nested *d, const struct w4 *s) { d->mid = *s; }

__attribute__((noinline)) static void off_both(struct nested *d, const struct nested *s) { d->mid = s->mid; }

/* the source is read again afterwards: its words are not dead at their stores */
__attribute__((noinline)) static uint32_t src_read_after(const struct w4 *p)
{
  struct w4 b;
  b = *p;
  return sum4(&b) * 3 + p->a[2];
}

/* the destination address is live past the copy */
__attribute__((noinline)) static uint32_t dst_live_after(struct w4 *d, const struct w4 *s)
{
  *d = *s;
  return sum4(d) + (d == s ? 1u : 0u);
}

/* a whole object onto itself */
__attribute__((noinline)) static uint32_t self_copy(struct w4 *p)
{
  *p = *p;
  return sum4(p);
}

/* more words than one LDM can hold */
__attribute__((noinline)) static uint32_t six_words(const struct w6 *p)
{
  struct w6 b;
  b = *p;
  uint32_t r = 0;
  for (int i = 0; i < 6; i++)
    r = r * 5 + b.a[i];
  return r;
}

/* the two halves of one object, overlapping in neither direction */
__attribute__((noinline)) static uint32_t halves(struct w6 *p)
{
  struct w3 lo, hi;
  lo = *(const struct w3 *)&p->a[0];
  hi = *(const struct w3 *)&p->a[3];
  return sum3(&lo) * 2 + sum3(&hi);
}

int main(void)
{
  struct w3 s3;
  struct w4 s4, d4;
  struct w6 s6;
  struct nested n, n2;

  fill3(&s3, 100);
  fill4(&s4, 200);
  printf("%u %u\n", ptr_to_local3(&s3), ptr_to_local4(&s4));

  local_to_ptr(&d4, 300);
  printf("%u\n", sum4(&d4));

  ptr_to_ptr(&d4, &s4);
  printf("%u\n", sum4(&d4));

  n.head = 1;
  fill4(&n.mid, 400);
  n.tail = 2;
  off_src(&d4, &n);
  printf("%u\n", sum4(&d4));

  off_dst(&n2, &s4);
  printf("%u\n", sum4(&n2.mid));

  n2.head = 0;
  n2.tail = 0;
  off_both(&n2, &n);
  printf("%u %u %u\n", sum4(&n2.mid), n2.head, n2.tail);

  printf("%u\n", src_read_after(&s4));
  printf("%u\n", dst_live_after(&d4, &s4));
  printf("%u\n", self_copy(&s4));

  for (int i = 0; i < 6; i++)
    s6.a[i] = (uint32_t)(500 + i);
  printf("%u %u\n", six_words(&s6), halves(&s6));
  return 0;
}
