/* Compile-only (test_codegen_asm.py::test_guard_head_keeps_func_name): a
 * guard head is replayed in the caller, where __func__ / __FUNCTION__ name
 * the caller.  touch() compares against its own name; replayed in main, the
 * test compared against "main" and skipped the call. */
int hits;
__attribute__((noinline)) void rec(int v) { hits = hits * 3 + v; }
#define WORK(v) rec(v); rec(1); rec(2); rec(3); rec(4); rec(5); rec(6); rec(7); rec(8)

static void touch(const char **slot)
{
  const char *me = __func__;
  if (*slot != me)
  {
    WORK(1);
    *slot = me;
  }
}

static void touch_upper(const char **slot)
{
  if (*slot != __FUNCTION__)
  {
    WORK(2);
    *slot = __FUNCTION__;
  }
}

/* Control: the same shape without the name still gets its guard head. */
static const char *const plain_name = "plain";
static void touch_plain(const char **slot)
{
  const char *me = plain_name;
  if (*slot != me)
  {
    WORK(3);
    *slot = me;
  }
}

void use_func(const char **s) { touch(s); }
void use_function(const char **s) { touch_upper(s); }
void use_plain(const char **s) { touch_plain(s); }
void use_func2(const char **s) { touch(s); }
void use_function2(const char **s) { touch_upper(s); }
void use_plain2(const char **s) { touch_plain(s); }
