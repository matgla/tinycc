/* A static nothing reads has its stores removed (dead_static_store) and is
   then collected from the object (gc_unreferenced_statics).  "Nothing reads"
   no longer rests on the frontend's addrtaken -- set by every `&x`, which
   zig's C output writes for every field store -- but on the function
   summaries and on the relocations of emitted data.  Every static below IS
   read, by a path those have to see; each must keep its stores. */
#include <stdio.h>
#include <string.h>

struct big { int v[12]; };

/* Read through a pointer handed to a function. */
static int via_call;
__attribute__((noinline)) int read_through(const int *p) { return *p; }

/* Read through a pointer stored in data. */
static int via_data;
static int *const via_data_ptr = &via_data;

/* Read by a function reached only through a table. */
static int via_table;
static int reads_via_table(void) { return via_table; }
static int (*const table[])(void) = {reads_via_table};

/* Written by a struct copy (a memmove call), read back by another. */
static struct big copied;
__attribute__((noinline)) struct big make(int k)
{
  struct big b;
  for (int i = 0; i < 12; i++)
    b.v[i] = k + i;
  return b;
}

/* Written through zig's field-store shape, read the same way. */
struct holder { int a; struct big b; };
static struct holder zig_shape;

/* Written, never read: its stores and the object go (checked by
   test_drop_unused_statics.py); the program cannot tell. */
static struct holder write_only;

int main(void)
{
  via_call = 11;
  via_data = 22;
  via_table = 33;
  copied = make(40);
  (*(&((struct holder *)&zig_shape)->a)) = 55;
  (*(&((struct holder *)&zig_shape)->b)) = make(60);
  (*(&((struct holder *)&write_only)->b)) = make(70);
  write_only.a = 77;

  struct big out;
  memcpy(&out, &copied, sizeof out);
  printf("via_call=%d via_data=%d via_table=%d\n", read_through(&via_call), *via_data_ptr, table[0]());
  printf("copied=%d,%d zig_shape=%d,%d\n", out.v[0], out.v[11], (*(&((struct holder *)&zig_shape)->a)),
         zig_shape.b.v[5]);
  return 0;
}
