/* Return classes (FuncAttr.func_ret_zext, ir/known_ext.c): a static function
   compiled earlier in the TU whose every return leaves r0 zero-extended
   publishes that, and a direct call to it drops the UXTB/UXTH of the result.
   AAPCS leaves r0's upper bits unspecified for a one- or two-byte composite
   (a Zig error union `struct { u16 }`), so the caller-side extension is only
   redundant when this TU compiled the callee and saw it.

   The shapes that must keep the extension: a call through a pointer, a
   callee defined after the caller, a callee whose r0 carries more than the
   composite (a three-byte struct), a non-static callee.  A self-recursive
   function may assume its own class while it is being computed. */
#include <stdio.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned u32;

struct eu
{
  u16 error;
};

struct eb
{
  u8 tag;
};

struct e3
{
  u8 a, b, c;
};

static volatile u32 vword = 0xDEAD0000u;
static const u16 codes[5] = {0, 3, 0xFFFF, 0x8000, 9};

static __attribute__((noinline)) struct eu leaf_load(int i)
{
  struct eu r;
  r.error = codes[i];
  return r;
}

/* The word's upper half is live garbage for a two-byte struct return. */
static __attribute__((noinline)) struct eu leaf_word(int i)
{
  u32 w = vword | codes[i];
  struct eu r;
  r.error = (u16)w;
  return r;
}

static __attribute__((noinline)) struct eb leaf_byte(int i)
{
  struct eb r;
  r.tag = (u8)(codes[i] + 200);
  return r;
}

static __attribute__((noinline)) struct e3 leaf_three(int i)
{
  struct e3 r;
  r.a = (u8)codes[i];
  r.b = 0xEE;
  r.c = (u8)i;
  return r;
}

static __attribute__((noinline)) int small_int(int i) { return codes[i] & 7; }

__attribute__((noinline)) struct eu extern_leaf(int i)
{
  struct eu r;
  r.error = codes[4 - i];
  return r;
}

static struct eu later(int i);

static __attribute__((noinline)) struct eu rec(int n)
{
  if (n <= 0)
  {
    struct eu z = {0};
    return z;
  }
  struct eu t = rec(n - 1);
  if (t.error)
    return t;
  return leaf_load(n % 5);
}

static struct eu (*volatile fp)(int) = leaf_word;

__attribute__((noinline)) u32 caller(int i)
{
  u32 acc = 0;
  struct eu a = leaf_load(i);
  if (a.error > 255)
    acc += 1000;
  acc += a.error;
  struct eu b = leaf_word(i);
  acc += b.error == codes[i];
  struct eb c = leaf_byte(i);
  acc += (u32)c.tag << 1;
  struct e3 d = leaf_three(i);
  acc += d.a + d.b + d.c;
  acc += (u32)small_int(i) << 20;
  struct eu e = fp(i);
  acc += e.error == codes[i] ? 7 : 0;
  struct eu f = later(i);
  acc += f.error ^ 5;
  struct eu g = extern_leaf(i);
  acc += g.error == codes[4 - i] ? 11 : 0;
  struct eu h = rec(i + 1);
  acc += (u32)h.error * 13u;
  return acc;
}

static __attribute__((noinline)) struct eu later(int i)
{
  u32 w = vword + (u32)i;
  struct eu r;
  r.error = (u16)w;
  return r;
}

int main(void)
{
  for (int i = 0; i < 5; i++)
    printf("caller(%d) = %u\n", i, caller(i));
  return 0;
}
