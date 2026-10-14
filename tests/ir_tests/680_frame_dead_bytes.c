/* Frame dead bytes (source/ir/frame_dfe.c): writes into frame bytes no
 * instruction reads are dropped, copies/fills/images shrink to the bytes
 * still read, and a frame object is cut into the pieces still referenced.
 * The shapes are the Zig C backend's (big values moved by value, 0xaa
 * "undefined" images, an optional read only for its tag) and the ones the
 * pass must leave alone: addresses that escape, copies out through
 * pointers, by-value arguments, volatile objects. */
#include <stdio.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned long long u64;

struct info
{
  u64 size;
  unsigned short date, time;
  u8 attr;
  u8 kind;
  char name[256];
  char alt[13];
};

struct opt_info
{
  struct info payload;
  u8 is_null;
};

struct eu_info
{
  struct info payload;
  unsigned short error;
};

static const struct info undef_info = {0xaaaaaaaaaaaaaaaaull, 0xaaaa, 0xaaaa, 0xaa, 0xaa, {(char)0xaa}, {(char)0xaa}};

__attribute__((noinline)) struct eu_info stat_like(int which)
{
  struct eu_info r;
  if (which < 0)
  {
    r.payload = undef_info;
    r.error = 7;
    return r;
  }
  memset(&r.payload, 0, sizeof r.payload);
  r.payload.size = 1000 + which;
  r.payload.kind = (u8)which;
  r.payload.attr = 0x20;
  strcpy(r.payload.name, "file.txt");
  r.error = 0;
  return r;
}

/* FatFs.get's shape: the result copied into an optional, only kind read. */
__attribute__((noinline)) int kind_of(int which)
{
  struct opt_info t14, t17;
  struct eu_info t15 = stat_like(which);
  if (t15.error == 0)
  {
    struct info t16 = t15.payload;
    t17.is_null = 0;
    t17.payload = t16;
    t14 = t17;
  }
  else
  {
    t14.is_null = 1;
    t14.payload = undef_info;
  }
  if (!t14.is_null)
  {
    struct info t18 = t14.payload;
    const struct info *t19 = &t18;
    return t19->kind == 1 ? 100 : t19->kind;
  }
  return -1;
}

/* Only the tail of a big copy is read: the copy keeps its tail. */
__attribute__((noinline)) int tail_of(const struct info *p)
{
  struct info a = *p, b;
  b = a;
  return b.alt[3] + b.alt[12] + (int)b.name[250];
}

/* Two distant parts read: the object is cut in two pieces. */
__attribute__((noinline)) unsigned two_ends(int k)
{
  struct info a;
  memset(&a, 0, sizeof a);
  a.size = (u64)k << 33 | 5;
  a.alt[11] = (char)(k + 1);
  a.name[100] = 9; /* never read */
  struct info b = a;
  return (unsigned)(b.size >> 32) + (unsigned)b.size + (unsigned)b.alt[11];
}

/* A constant image whose middle is all that is read. */
__attribute__((noinline)) int image_mid(int i)
{
  struct
  {
    int pad0[20];
    int mid[8];
    int pad1[20];
  } img = {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20},
           {21, 22, 23, 24, 25, 26, 27, 28},
           {29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48}};
  img.pad1[3] = i;
  return img.mid[0] + img.mid[7] + img.mid[i & 3];
}

/* Escapes: every byte stays. */
__attribute__((noinline)) void fill(struct info *p, int v)
{
  p->size = v;
  p->name[200] = (char)v;
}

__attribute__((noinline)) int escaped_to_call(int v)
{
  struct info a;
  memset(&a, 0x11, sizeof a);
  fill(&a, v);
  return a.name[200] + (int)a.size + a.alt[5];
}

static struct info *saved;
__attribute__((noinline)) int read_saved(void)
{
  return saved->alt[7] + saved->name[3];
}

__attribute__((noinline)) int escaped_to_global(int v)
{
  struct info a;
  memset(&a, v, sizeof a);
  saved = &a;
  int r = read_saved();
  saved = 0;
  return r;
}

/* Leaves junk where the next call's frame will be, so bytes a wrong
 * deletion leaves unwritten do not read back as zero by luck. */
__attribute__((noinline)) int scribble(int v)
{
  volatile unsigned char junk[1024];
  for (int k = 0; k < 1024; k++)
    junk[k] = (unsigned char)(0x5a + v);
  return junk[v & 1023];
}

/* A copy out through a pointer reads its whole source. */
__attribute__((noinline)) void copy_out(struct info *out, int v)
{
  struct info a;
  memset(&a, 0, sizeof a);
  a.name[255] = (char)v;
  a.alt[0] = (char)(v + 1);
  *out = a;
}

/* By-value argument: the callee reads it all. */
__attribute__((noinline)) int sum_byval(struct info a)
{
  return a.alt[12] + a.name[128] + a.kind;
}

__attribute__((noinline)) int byval_arg(int v)
{
  struct info a;
  memset(&a, 0, sizeof a);
  a.name[128] = (char)v;
  a.alt[12] = 3;
  a.kind = 4;
  struct info b = a;
  return sum_byval(b);
}

/* Indexed reads: the address is offset by a variable, nothing is dropped. */
__attribute__((noinline)) int indexed(int i)
{
  struct info a;
  memset(&a, 0, sizeof a);
  for (int k = 0; k < 256; k++)
    a.name[k] = (char)(k * 3);
  struct info b = a;
  return b.name[i] + b.name[i + 7];
}

/* memcmp reads its arguments through a call. */
__attribute__((noinline)) int compared(int v)
{
  struct info a, b;
  memset(&a, 0, sizeof a);
  memset(&b, 0, sizeof b);
  a.name[77] = (char)v;
  return memcmp(&a, &b, sizeof a) != 0;
}

/* A volatile object keeps its stores. */
__attribute__((noinline)) int volatile_obj(int v)
{
  volatile int arr[40];
  for (int k = 0; k < 40; k++)
    arr[k] = k + v;
  return arr[3];
}

/* Loop-carried: the bytes written in one iteration are read in the next. */
__attribute__((noinline)) int carried(int n)
{
  struct info a, b;
  memset(&a, 0, sizeof a);
  int s = 0;
  for (int k = 0; k < n; k++)
  {
    b = a;
    s += b.alt[2] + b.name[40];
    a.alt[2] = (char)k;
    a.name[40] = (char)(2 * k);
  }
  return s;
}

/* 8-byte fields copied with memmove8 between pieces. */
struct d8
{
  double a;
  u64 b;
  char filler[200];
  u64 c;
  double d;
};

__attribute__((noinline)) double d8_fields(double x)
{
  struct d8 s, t;
  memset(&s, 0, sizeof s);
  s.a = x;
  s.c = 3;
  s.d = x * 2;
  s.filler[5] = 1; /* never read */
  t = s;
  struct d8 u = t;
  return u.d + (double)u.c + u.a;
}

/* An unaligned byte source copied into a local, part of it read. */
__attribute__((noinline)) unsigned unaligned_src(const u8 *buf)
{
  struct
  {
    unsigned a, b, c, d;
    u8 rest[48];
  } m;
  memcpy(&m, buf + 1, sizeof m);
  return m.d + m.rest[47];
}

/* A _Complex member is read and written whole: both halves. */
struct cpx
{
  char pad[80];
  double _Complex z;
  int tag;
};

__attribute__((noinline)) double cpx_part(double _Complex z)
{
  struct cpx a, b;
  /* (a 64-byte struct here, or a memset of `a` first, loses the _Complex
   * store at -O1+ with or without this pass: dead_local_slot, a separate
   * bug) */
  a.pad[3] = 1;
  a.z = z;
  a.tag = 3;
  b = a;
  double _Complex w = b.z; /* (__imag__ of a struct member itself reads the real half in tcc) */
  return __imag__ w * 10 + __real__ w + b.tag;
}

int main(void)
{
  printf("kind %d %d %d %d\n", kind_of(1), kind_of(2), kind_of(-1), kind_of(0));
  struct info in;
  memset(&in, 0, sizeof in);
  in.alt[3] = 4;
  in.alt[12] = 5;
  in.name[250] = 6;
  printf("tail %d\n", tail_of(&in));
  printf("two_ends %u %u\n", two_ends(3), two_ends(70));
  printf("image %d %d\n", image_mid(1), image_mid(6));
  printf("escaped %d %d\n", escaped_to_call(5), escaped_to_global(2));
  struct info out;
  memset(&out, 0x55, sizeof out);
  scribble(1);
  copy_out(&out, 9);
  printf("copy_out %d %d %d %d\n", out.name[255], out.alt[0], out.name[3], out.alt[12]);
  scribble(2);
  printf("byval %d\n", byval_arg(8));
  printf("indexed %d\n", indexed(10));
  printf("compared %d %d\n", compared(0), compared(3));
  printf("volatile %d\n", volatile_obj(2));
  printf("carried %d\n", carried(5));
  printf("d8 %d\n", (int)d8_fields(1.5));
  u8 buf[80];
  for (int k = 0; k < 80; k++)
    buf[k] = (u8)(k + 1);
  printf("unaligned %u\n", unaligned_src(buf));
  printf("cpx %d\n", (int)cpx_part(2.0 + 3.0 * 1.0i));
  return 0;
}
