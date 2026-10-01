/* -fdrop-unused-statics defers static functions and initialized static objects
   to the end of the TU and emits only those something live refers to.  Each
   shape below is one the deferral has to get right: what is used must still
   be defined, with its bytes, where the code that reads it can see them. */
#include <stdio.h>
#include <string.h>

/* Forward-declared const object defined later (zig's C output does this for
   every constant): the tentative must wait in COMMON, not take .rodata bytes
   the definition then fills. */
struct pair { int a, b; };
static const struct pair fwd_const;
static int read_fwd_const(void) { return fwd_const.a * 10 + fwd_const.b; }
static const struct pair fwd_const = {4, 2};

/* Unsized arrays: the saved initializer sizes them at the declaration, so a
   sizeof further down (even at file scope) sees the complete type. */
static const char greeting[] = "deferred";
static const int greeting_len = sizeof(greeting);
static const int squares[] = {0, 1, 4, 9, 16};

/* Liveness through data: a table of function pointers reached only from a
   function that is itself reached only through another table. */
static int twice(int x) { return 2 * x; }
static int thrice(int x) { return 3 * x; }
static int (*const ops[])(int) = {twice, thrice};
static int apply_ops(int x) { return ops[0](x) + ops[1](x); }
struct vt { int (*run)(int); };
static const struct vt vtable = {apply_ops};
static const struct vt *const vtable_ptr = &vtable;

/* A block-scope prototype is a declaration, not a call to an unprototyped
   function: the body stays deferred. */
static int via_local_decl(int x)
{
  extern size_t strlen(const char *);
  return x + (int)strlen(greeting);
}

/* Kept whatever refers to it. */
__attribute__((used)) static int used_counter = 11;
static int alias_target(void) { return 99; }
int aliased(void) __attribute__((alias("alias_target")));

/* Unused: dropped (the object-level check is test_drop_unused_statics.py). */
static int never_called(int x) { return x + fwd_const.b; }
static const int never_read[4] = {1, 2, 3, 4};

/* A static written in one place and read in another, both deferred. */
static int counter;
static void bump(void) { counter += 3; }

int main(void)
{
  bump();
  bump();
  printf("fwd_const=%d\n", read_fwd_const());
  printf("greeting=%s len=%d sq=%d n=%d\n", greeting, greeting_len, squares[4],
         (int)(sizeof(squares) / sizeof(squares[0])));
  printf("vtable=%d\n", vtable_ptr->run(5));
  printf("local_decl=%d\n", via_local_decl(1));
  printf("used=%d aliased=%d counter=%d\n", used_counter, aliased(), counter);
  return 0;
}
