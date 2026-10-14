/* ra:reload_elim must not delete a volatile load of a frame slot.
 *
 * After allocation ra_redundant_reload_elim remembers that a frame slot holds
 * the register it was just stored from and NOPs a later load of that slot into
 * the same register.  It checked neither the store it recorded nor the load it
 * deleted for volatility, so `v[1] = x; return v[1];` on a volatile local
 * array (or volatile struct member) compiled to a bare `str`: the read the
 * standard mandates never happened.  A volatile scalar VAR kept its load only
 * because it is a vreg operand the pass does not track.
 *
 * Nothing in a QEMU run can write a frame slot behind the compiler's back, so
 * the program below only checks the values; the failing half of this test is
 * test_codegen_asm.py::test_bug_ra_reload_elim_volatile_frame_load, which
 * counts the real ldr/str of every function here at -O1/-O2/-Os.  Keep the
 * function names and the WANT table in that test in step.
 *
 * Every function stores a register-resident value (the parameter) to the slot
 * and reads it back through a volatile access, the shape gcc compiles to
 * `str; ldr`. */
#include <stdio.h>

__attribute__((noinline)) int vol_array_elem(int x)
{
  volatile int v[2];
  v[1] = x;
  return v[1];
}

__attribute__((noinline)) int vol_struct_obj(int x)
{
  volatile struct
  {
    int a, b;
  } s;
  s.a = x;
  return s.a;
}

__attribute__((noinline)) int vol_struct_member(int x)
{
  struct
  {
    volatile int a;
    int b;
  } s;
  s.a = x;
  return s.a;
}

__attribute__((noinline)) long long vol_array_elem64(long long x)
{
  volatile long long v[2];
  v[1] = x;
  return v[1];
}

__attribute__((noinline)) double vol_double(double x)
{
  volatile double v[2];
  v[0] = x;
  return v[0];
}

/* a plain store, then a volatile load of the same slot: the load is the one
 * the standard mandates, whoever wrote the bytes */
__attribute__((noinline)) int plain_store_volatile_load(int x)
{
  union
  {
    int n;
    volatile int v;
  } u;
  u.n = x;
  return u.v;
}

/* two reads of one volatile slot are two accesses */
__attribute__((noinline)) int vol_read_twice(int x)
{
  volatile int v[2];
  v[0] = x;
  return v[0] + v[0];
}

static int bad;

static void check(const char *name, long long got, long long want)
{
  if (got != want)
  {
    printf("%s: got %lld want %lld\n", name, got, want);
    bad = 1;
  }
  else
    printf("%s ok\n", name);
}

int main(void)
{
  check("vol_array_elem", vol_array_elem(0x1234), 0x1234);
  check("vol_struct_obj", vol_struct_obj(-77), -77);
  check("vol_struct_member", vol_struct_member(31337), 31337);
  check("vol_array_elem64", vol_array_elem64(0x1122334455667788LL), 0x1122334455667788LL);
  check("vol_double", (long long)vol_double(2048.0), 2048);
  check("plain_store_volatile_load", plain_store_volatile_load(99), 99);
  check("vol_read_twice", vol_read_twice(21), 42);
  return bad;
}
