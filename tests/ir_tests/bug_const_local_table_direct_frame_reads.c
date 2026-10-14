/* A constant local aggregate read only by constant indices is written by one
   run of constant stores and read through its own frame slots: the reads
   belong to its image, so the stores and the stack slots are dead.  The
   shapes that must keep the stack copy: an element written after the
   initialiser, and the object's address flowing into a call. */
#include <stdint.h>

typedef int fh(int);

static int f0(int x) { return x + 1; }
static int f1(int x) { return x + 2; }
static int f2(int x) { return x + 3; }
static int f3(int x) { return x + 4; }
static int f4(int x) { return x + 5; }
static int f5(int x) { return x + 6; }
static int f6(int x) { return x + 7; }
static int f7(int x) { return x + 8; }

int through(fh *t[]);

/* Only constant indices: no image, no frame today. */
__attribute__((noinline)) int onlyconst(unsigned n)
{
    fh *t[8] = { f0, f1, f2, f3, f4, f5, f6, f7 };
    return t[2](1) + t[5](n);
}

/* A dynamic index alongside a constant one: the table is read from an image,
   but the constant read still stores through the frame slot. */
__attribute__((noinline)) int mixed(unsigned n)
{
    fh *t[8] = { f0, f1, f2, f3, f4, f5, f6, f7 };
    return t[n & 7u](1) + t[3](2);
}

/* Must stay on the stack: an element is written after the initialiser. */
__attribute__((noinline)) int written(unsigned n)
{
    fh *t[8] = { f0, f1, f2, f3, f4, f5, f6, f7 };
    t[6] = f1;
    return t[n & 7u](1) + t[2](2);
}

/* Must stay on the stack: the table's address is passed to a call. */
int through(fh *t[])
{
    return t[0](0) + t[7](0);
}
__attribute__((noinline)) int escapes(unsigned n)
{
    fh *t[8] = { f0, f1, f2, f3, f4, f5, f6, f7 };
    return through(t) + t[n & 7u](1);
}

int main(void)
{
    return onlyconst(3) + mixed(3) + written(6) + escapes(2) != 45;
}
