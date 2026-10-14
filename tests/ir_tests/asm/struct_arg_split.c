/* ra:struct_arg_split shape cases: by-value struct arguments (the Zig C
 * backend's slices and tuples) are rebuilt through frame slots by the front
 * end.  They must reach the call as plain words in registers: no slot, no
 * stores, and where nothing else is needed a tail call.  Each wrapper takes
 * its words as scalars so nothing folds. */
typedef unsigned int u32;
struct sl { unsigned char *p; u32 n; };
struct tup { void *f0; struct sl f1; };
struct tup1 { struct sl f0; };
struct w1 { u32 a; };
struct w3 { u32 a, b, c; };
struct w4 { u32 a, b, c, d; };

extern long rd(void *s, struct sl b);
extern long rd1(void *s, struct w1 b);
extern long rd3(void *s, struct w3 b);
extern long rd4(void *s, struct w4 b);
extern long rd_after(void *s, struct sl b, u32 x);
extern long rd_r3(u32 x, u32 y, u32 z, struct sl b);

/* Zig-style tuple round trip (gap/r1.c). */
long call_tuple(void *a0, struct sl a1)
{
  struct tup1 t0;
  struct tup t5;
  struct sl t4;
  void *t2;
  t0.f0 = a1;
  t2 = a0;
  t4 = t0.f0;
  t5.f0 = t2;
  t5.f1 = t4;
  t2 = t5.f0;
  t4 = t5.f1;
  return rd(t2, t4);
}

long call_sl(void *a, unsigned char *p, u32 n)
{
  struct sl s = {p, n};
  return rd(a, s);
}

long call_w1(void *a, u32 x)
{
  struct w1 s = {x};
  return rd1(a, s);
}

long call_w3(void *a, u32 x, u32 y, u32 z)
{
  struct w3 s = {x, y, z};
  return rd3(a, s);
}

/* 16 bytes: r1-r3 and one word on the stack -- the argument straddles r3. */
long call_w4(void *a, u32 x, u32 y, u32 z, u32 t)
{
  struct w4 s = {x, y, z, t};
  return rd4(a, s);
}

/* An argument after the struct. */
long call_after(void *a, unsigned char *p, u32 n, u32 x)
{
  struct sl s = {p, n};
  return rd_after(a, s, x);
}

/* The struct starts in r3. */
long call_r3(u32 x, u32 y, u32 z, unsigned char *p, u32 n)
{
  struct sl s = {p, n};
  return rd_r3(x, y, z, s);
}
