// The YasOS kernel through Zig's C backend at -O2: Zig reaches an interface
// through a zero-size field at the END of the 28-byte interface struct, so
// the caller passes `&local + 28` and the callee steps back 28 bytes to the
// vtable.  Once the callee is inlined, its pointer parameter lands as a
// VAR -> VAR `V79 <-- V22 [STORE]`.  dse's address propagation did not follow
// that store while its read scan counted it as a safe copy, so the loads
// through `param - 28` never reached the local: the copy into it died and the
// vtable call jumped through stack garbage (VFS mount hung at boot).
#include <stdio.h>
#include <string.h>

struct handle; /* the zero-size field's type */

struct vtable
{
  int (*get)(void *ctx, int x);
  int (*put)(void *ctx, int x);
};

struct iface
{
  const struct vtable *vtable;
  void *ptr;
  int mem[5];
}; /* 28 bytes, the handle sits at offset 28 */

struct mount
{
  int flags;
  int id;
  struct iface fs;
};

static int add_get(void *ctx, int x) { return *(int *)ctx + x; }
static int add_put(void *ctx, int x) { return *(int *)ctx - x; }
static const struct vtable add_vt = {add_get, add_put};

static int iface_get(struct handle *self, int x)
{
  const struct iface *p = (const struct iface *)((char *)self - 28);
  return p->vtable->get(p->ptr, x);
}

static int iface_put(struct handle *self, int x)
{
  const struct iface *p = (const struct iface *)((char *)self - 28);
  return p->vtable->put(p->ptr, x) + p->mem[4];
}

__attribute__((noinline)) static int use(const struct mount *m, int x)
{
  struct iface t30, t8;
  struct handle *t35;
  t30 = m->fs;
  t8 = t30;
  t35 = (struct handle *)((char *)&t8 + 28);
  return iface_get(t35, x);
}

__attribute__((noinline)) static int use_two(const struct mount *m, int x)
{
  struct iface t30, t8;
  struct handle *t35;
  t30 = m->fs;
  t8 = t30;
  t35 = (struct handle *)((char *)&t8 + 28);
  int a = iface_get(t35, x);
  return a + iface_put(t35, x);
}

/* Leave garbage where use()'s frame will be, so a dropped copy cannot read
 * a stale-but-correct value. */
__attribute__((noinline)) static void dirty_stack(void)
{
  volatile unsigned char junk[512];
  memset((void *)junk, 0xA5, sizeof junk);
}

int main(void)
{
  int base = 40;
  struct mount m = {1, 7, {&add_vt, &base, {0, 0, 0, 0, 3}}};
  dirty_stack();
  printf("get=%d\n", use(&m, 2));
  dirty_stack();
  printf("two=%d\n", use_two(&m, 2));
  return 0;
}
