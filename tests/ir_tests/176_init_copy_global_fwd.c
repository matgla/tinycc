/* Regression guard for init-copy-from-global load forwarding
 * (tcc_ir_opt_memmove_global_load_fwd, ir/opt_memory.c).
 *
 * `struct y = global; ... return y.field;` copies the global into a private
 * read-only stack slot, then reads a few fields.  The pass rewrites the loads
 * to read the global directly and drops the dead copy.  These checks pin:
 *   (a) CORRECTNESS of the forwarded reads at several field offsets/widths and
 *       for multiple fields read from one copy;
 *   (b) the SAFETY gate that must keep the pass OFF a snapshot whose source is
 *       mutated after the copy — `x = g; mutate(g); use x` must still observe
 *       the PRE-mutation value of x (a call separates copy from reads, so the
 *       no-call/no-store window gate forbids forwarding).  If the pass wrongly
 *       fired here it would read the post-mutation global and the snapshot
 *       check would fail.
 */
#include <stdio.h>

struct S { unsigned int a, b, c; };
struct T { unsigned short h0, h1; unsigned char b4, b5; };

struct S gS;
struct T gT;

/* forwardable: copy + single field read, no escape, no intervening call */
unsigned int get_b(void) { struct S y = gS; return y.b; }
unsigned int get_c(unsigned int x) { struct S y = gS; return y.c + x; }
/* multiple fields read from one copy */
unsigned int sum3(void) { struct S y = gS; return y.a + y.b + y.c; }
/* narrower fields */
unsigned int get_h1(void) { struct T y = gT; return y.h1; }
unsigned int get_b5(void) { struct T y = gT; return y.b5; }

/* The snapshot must NOT be forwarded: bump() mutates gS between copy and read. */
void bump(void) { gS.a += 100; gS.b += 100; gS.c += 100; }
int snapshot_ok(void)
{
  struct S x = gS;       /* snapshot of {1,2,3} */
  bump();                /* gS becomes {101,102,103} — a call after the copy */
  return x.a == 1 && x.b == 2 && x.c == 3;  /* x must keep the old values */
}

int main(void)
{
  int ok = 1;

  gS.a = 1; gS.b = 2; gS.c = 3;
  gT.h0 = 0x1111; gT.h1 = 0xbeef; gT.b4 = 0x5a; gT.b5 = 0xa5;

  if (get_b() != 2) ok = 0;
  if (get_c(40) != 43) ok = 0;
  if (sum3() != 6) ok = 0;
  if (get_h1() != 0xbeef) ok = 0;
  if (get_b5() != 0xa5) ok = 0;
  if (!snapshot_ok()) ok = 0;

  printf("%s\n", ok ? "OK" : "FAIL");
  return 0;
}
