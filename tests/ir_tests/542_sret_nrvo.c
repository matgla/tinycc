/* A struct returned through the hidden pointer is built in the caller's buffer
   when the local holding it -- and a local copied into it whole, which then
   dies -- is reached only through its own address, and nothing else touches
   memory while it is built: Zig's `t0 = {...}; ...; t10 = t0; return t10;`
   in every Wyhash.init.  The caller may pass a buffer the callee sees through
   another pointer (`*x = f(x)`), so a read or write through a pointer, or a
   call, while the object is built keeps the local.  Here: the Wyhash.init
   shape (zero fill, 64-bit fields, a copy from .rodata, a whole-object copy
   between two locals), and callees that read their argument after writing
   part of the result, with the caller passing that argument as the buffer. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

struct wy { uint64_t a, b, state[3]; uintptr_t total, blen; uint8_t buf[48]; };

__attribute__((noinline)) static struct wy wy_init(uint64_t seed)
{
    struct wy t0 = {0x1111, 0x2222, {3, 4, 5}, 0, 0, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV"};
    struct wy t10;
    uint64_t *p = &t0.state[0];
    *p = seed ^ 0x9e3779b97f4a7c15ull;
    t0.state[1] = t0.state[0] * 3;
    t0.state[2] = t0.state[1] + t0.a;
    t10 = t0;
    return t10;
}

struct pair { unsigned x, y, z, w, pad[4]; };

/* reads *in after writing r.x: x aliases the result when called as *p = swap(p) */
__attribute__((noinline)) static struct pair swap(const struct pair *in)
{
    struct pair r;
    r.x = in->y;
    r.y = in->x;
    r.z = in->w + in->x;
    r.w = in->z;
    r.pad[0] = r.pad[1] = r.pad[2] = r.pad[3] = in->pad[0] + 1;
    return r;
}

static struct pair g;

/* a global read while building */
__attribute__((noinline)) static struct pair from_global(unsigned k)
{
    struct pair r;
    r.x = k;
    r.y = g.x + k;
    r.z = g.y;
    r.w = 7;
    r.pad[0] = r.pad[1] = r.pad[2] = r.pad[3] = g.pad[0] + 1;
    return r;
}

/* a call while building */
__attribute__((noinline)) static unsigned peek(const struct pair *p) { return p->x * 10 + p->y; }
__attribute__((noinline)) static struct pair with_call(const struct pair *in)
{
    struct pair r;
    r.x = 1;
    r.y = 2;
    r.z = peek(in);
    r.w = in->x;
    r.pad[0] = r.pad[1] = r.pad[2] = r.pad[3] = 0;
    return r;
}

int main(void)
{
    struct wy w = wy_init(42);
    printf("wy %llx %llx %llx %llx %llx %u %u %.5s %.5s\n", (unsigned long long)w.a, (unsigned long long)w.b,
           (unsigned long long)w.state[0], (unsigned long long)w.state[1], (unsigned long long)w.state[2],
           (unsigned)w.total, (unsigned)w.blen, (const char *)w.buf, (const char *)w.buf + 43);
    struct pair p = {1, 2, 3, 4, {5}};
    p = swap(&p);
    printf("swap %u %u %u %u %u\n", p.x, p.y, p.z, p.w, p.pad[3]);
    g = (struct pair){5, 6, 7, 8, {9}};
    g = from_global(3);
    printf("global %u %u %u %u %u\n", g.x, g.y, g.z, g.w, g.pad[3]);
    struct pair q = {9, 8, 7, 6, {1}};
    q = with_call(&q);
    printf("call %u %u %u %u\n", q.x, q.y, q.z, q.w);
    return 0;
}
