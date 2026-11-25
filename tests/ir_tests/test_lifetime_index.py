"""-Wlifetime: local arrays indexed by a variable (lifetime.c).

- Maybe members of an element at a variable index: `a[i].next` of a local or
  global array of structs, `p[i].next` through a pointer.  The raw IR
  computes the address again at each use (`i SHL #k` / `i MUL #size`, added
  to the base); the path is keyed by (base, member offset, index variable)
  and forgotten when the index variable is written.  `if (a[i].next)
  a[i].next->v` is clean, untested it may be null.  `a[i]` and `a[j]` are
  not assumed to be the same element, nor `a[i]` and `a[1]`: a store
  through one does not change what is known of the other.
- Owned elements at a variable index (`char * owned a[N]`, `b[i].buf`,
  `int owned(close) fds[N]`): per array and index variable an item 'a[i]'
  stands for the element at i while i is not written; writing i joins it
  into every element (vague: only definite states are reported there).
  Loops over i are summarised: `for (i = C0; i < N; i++) a[i] = malloc(...)`
  then `for (...) free(a[i])` is clean; without the free loop the elements
  leak ("may", said once as 'a[i]'); freeing a[i] and using it with i
  unchanged is a use after free.  A loop that is not `for (i = C0; i < N;
  i++)` over the array (a variable bound, a break) joins its result into
  every element: no definite report comes from it.  With two index
  variables an element the loops left owned is said by its own name.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

PRELUDE = """\
#pragma yasc
void * owned maybe malloc(unsigned n);
void free(drop void * owned maybe p);
int use(char * view p);
int close(int drop owned(close) maybe fd);
int owned(close) maybe open(const char * view p, int f);
int pipe(out int owned(close) maybe * view fds);
struct box { char * owned maybe buf; int n; };
struct node { int v; struct node * view maybe next; };
void fill(inout struct node * view a, int n);
struct node g[4];
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _compile(tmp_path, body, *flags, obj="t.o"):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", "-c", *flags, str(src), "-o", str(tmp_path / obj)],
        capture_output=True,
        text=True,
    )


def _diags(tmp_path, body, *flags):
    proc = _compile(tmp_path, body, *flags)
    assert proc.returncode == 0, proc.stderr
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == "warning":
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[2]))
    return out


# ---- maybe members at a variable index -------------------------------------

MEMBER_CLEAN = {
    "tested": """\
int f(int i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  if (a[i].next)
    return a[i].next->v;
  return 0;
}
""",
    "tested_negated": """\
int f(int i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  if (!a[i].next)
    return 0;
  return a[i].next->v;
}
""",
    "loop": """\
int f(void)
{
  struct node a[4] = undefined;
  int s = 0;
  fill(a, 4);
  for (int i = 0; i < 4; i++)
    if (a[i].next)
      s += a[i].next->v;
  return s;
}
""",
    "pointer": """\
int f(struct node * view p, int i)
{
  if (p[i].next)
    return p[i].next->v;
  return 0;
}
""",
    "global": """\
int f(int i)
{
  if (g[i].next)
    return g[i].next->v;
  return 0;
}
""",
    "char_index": """\
int f(char i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  if (a[i].next)
    return a[i].next->v;
  return 0;
}
""",
    "long_long_index": """\
int f(long long i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  if (a[i].next)
    return a[i].next->v;
  return 0;
}
""",
    "copied": """\
int f(int i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  struct node * view maybe n = a[i].next;
  if (n)
    return a[i].next->v;
  return 0;
}
""",
    # a store through another element is assumed not to be this one
    "other_element_stored": """\
int f(int i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  if (a[i].next)
  {
    a[1].next = 0;
    return a[i].next->v;
  }
  return 0;
}
""",
    "while_tested": """\
int f(int i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  while (i < 4 && a[i].next)
    i++;
  return a[i].next ? a[i].next->v : 0;
}
""",
}

MEMBER_BAD = {
    "untested": ("""\
int f(int i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  return a[i].next->v;
}
""", 5, "'a[i].next' may be null where it is dereferenced"),
    "index_written": ("""\
int f(int i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  if (a[i].next)
  {
    i++;
    return a[i].next->v;
  }
  return 0;
}
""", 8, "'a[i].next' may be null where it is dereferenced"),
    "other_index": ("""\
int f(int i, int j)
{
  struct node a[4] = undefined;
  fill(a, 4);
  if (a[i].next)
    return a[j].next->v;
  return 0;
}
""", 6, "'a[j].next' may be null where it is dereferenced"),
    "stored_null": ("""\
int f(int i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  a[i].next = 0;
  return a[i].next->v;
}
""", 6, "'a[i].next' is null where it is dereferenced"),
    "pointer_untested": ("""\
int f(struct node * view p, int i)
{
  if (p[i].next)
    return p[i].next->v;
  return p[i].next->v;
}
""", 5, "'p[i].next' is null where it is dereferenced"),
    "global_untested": ("""\
int f(int i)
{
  return g[i].next->v;
}
""", 3, "'g[i].next' may be null where it is dereferenced"),
    # the array handed to a call that may write it: tested again
    "forgotten_by_call": ("""\
int f(int i)
{
  struct node a[4] = undefined;
  fill(a, 4);
  if (a[i].next)
  {
    fill(a, 4);
    return a[i].next->v;
  }
  return 0;
}
""", 8, "'a[i].next' may be null where it is dereferenced"),
}


@pytest.mark.parametrize("name", sorted(MEMBER_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_member_clean(tmp_path, name, opt):
    assert _diags(tmp_path, MEMBER_CLEAN[name], opt) == []


@pytest.mark.parametrize("name", sorted(MEMBER_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_member_reported(tmp_path, name, opt):
    body, line, msg = MEMBER_BAD[name]
    assert _diags(tmp_path, body, opt) == [(line, msg)]


# ---- owned elements at a variable index ---------------------------------------

OWNED_CLEAN = {
    "alloc_then_free_loops": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""",
    "declared_in_for": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  for (int i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (unsigned i = 0; i <= 3; ++i)
    free(a[i]);
}
""",
    "counting_down": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  for (int i = 3; i >= 0; i--)
    a[i] = malloc(4);
  for (int i = 0; i < 4; i++)
    free(a[i]);
}
""",
    "struct_member": """\
void f(void)
{
  struct box b[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    b[i].buf = malloc(4);
  for (i = 0; i < 4; i++)
    free(b[i].buf);
}
""",
    "handles": """\
void f(void)
{
  int owned(close) maybe fds[2] = undefined;
  int i;
  for (i = 0; i < 2; i++)
    fds[i] = open("x", 0);
  for (i = 0; i < 2; i++)
    close(fds[i]);
}
""",
    "pipe_then_loop": """\
void f(void)
{
  int owned(close) maybe fds[2] = undefined;
  if (pipe(fds) < 0)
    return;
  for (int i = 0; i < 2; i++)
    close(fds[i]);
}
""",
    "variable_bound": """\
void f(int n)
{
  char * owned maybe a[8] = undefined;
  int i;
  for (i = 0; i < n; i++)
    a[i] = malloc(4);
  for (i = 0; i < n; i++)
    free(a[i]);
}
""",
    "two_index_variables": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i, j;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (j = 0; j < 4; j++)
    free(a[j]);
}
""",
    "constant_store_then_loops": """\
void f(void)
{
  char * owned maybe a[5] = undefined;
  int i;
  a[4] = 0;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""",
    "constant_alloc_then_loops": """\
void f(void)
{
  char * owned maybe a[5] = undefined;
  int i;
  a[4] = malloc(3);
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    free(a[i]);
  free(a[4]);
}
""",
    "second_for_i_full_free": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  for (int i = 0; i < 4; i++)
    a[i] = malloc(8);
  for (int i = 0; i < 4; i++)
    free(a[i]);
}
""",
    "tested_in_loop": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    if (a[i])
      use(a[i]);
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""",
    "break_on_failure": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
  {
    a[i] = malloc(4);
    if (!a[i])
      break;
  }
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""",
    "goto_cleanup": """\
int f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
  {
    a[i] = malloc(4);
    if (!a[i])
      goto fail;
  }
  for (i = 0; i < 4; i++)
    free(a[i]);
  return 0;
fail:
  while (i > 0)
  {
    i--;
    free(a[i]);
  }
  return -1;
}
""",
    "cleanup_on_failure": """\
int f(void)
{
  char * owned maybe a[4] = undefined;
  int i, j;
  for (i = 0; i < 4; i++)
  {
    a[i] = malloc(4);
    if (!a[i])
    {
      for (j = 0; j < i; j++)
        free(a[j]);
      return -1;
    }
  }
  for (i = 0; i < 4; i++)
    free(a[i]);
  return 0;
}
""",
    "moved_out": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
  {
    char * owned maybe p = move(a[i]);
    free(p);
  }
}
""",
    "freed_once_each": """\
void f(int i)
{
  char * owned maybe a[4] = undefined;
  a[i] = malloc(4);
  free(a[i]);
}
""",
    "constant_then_loop": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = 0;
  a[1] = malloc(3);
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""",
    "member_array": """\
struct s { char * owned maybe p[4]; int n; };
void f(void)
{
  struct s x = undefined;
  int i;
  for (i = 0; i < 4; i++)
    x.p[i] = malloc(4);
  for (i = 0; i < 4; i++)
    free(x.p[i]);
}
""",
    "two_dimensions": """\
void f(void)
{
  char * owned maybe a[2][3] = undefined;
  int i;
  for (i = 0; i < 3; i++)
    a[1][i] = malloc(4);
  for (i = 0; i < 3; i++)
    free(a[1][i]);
}
""",
    "loop_with_continue": """\
void f(int n)
{
  char * owned maybe a[4] = undefined;
  int i = 0;
  while (i < n)
  {
    if (i == 2)
    {
      i++;
      continue;
    }
    a[i] = malloc(1);
    i++;
  }
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""",
    # an index plus a constant
    "shifted_free_loop": """\
void f(void)
{
  char * owned maybe a[5] = undefined;
  int i;
  for (i = 0; i < 5; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    free(a[i + 1]);
  free(a[0]);
}
""",
    "shifted_alloc_loop": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 1; i < 5; i++)
    a[i - 1] = malloc(4);
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""",
    "neighbours": """\
void f(int i)
{
  char * owned maybe a[4] = undefined;
  a[i] = malloc(4);
  a[i + 1] = malloc(4);
  free(a[i + 1]);
  free(a[i]);
}
""",
    "shifted_member": """\
void f(void)
{
  struct box b[3] = undefined;
  int i;
  for (i = 1; i < 4; i++)
    b[i - 1].buf = malloc(4);
  for (i = 0; i < 3; i++)
    free(b[i].buf);
}
""",
    # past the array: not summarised, nothing definite
    "shifted_past_the_end": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    free(a[i + 1]);
}
""",
    # a[i] and a[i + 1] in one loop: not summarised
    "two_offsets_in_a_loop": """\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 3; i++)
  {
    free(a[i]);
    use(a[i + 1]);
  }
  free(a[3]);
}
""",
    # i == j returned before: a[i] and a[j] are different elements
    "swap_remove": """\
void f(int i, int j)
{
  char * owned maybe a[4] = {0};
  for (int k = 0; k < 4; k++)
    a[k] = malloc(4);
  if (i < 0 || i >= 4 || j < 0 || j >= 4 || i == j)
  {
    for (int k = 0; k < 4; k++)
      free(a[k]);
    return;
  }
  free(a[i]);
  a[i] = move(a[j]);
  a[j] = 0;
  for (int k = 0; k < 4; k++)
    free(a[k]);
}
""",
    "under_i_ne_j": """\
void f(int i, int j)
{
  char * owned maybe a[2] = undefined;
  a[0] = malloc(4);
  a[1] = malloc(4);
  if (i != j && i >= 0 && i < 2 && j >= 0 && j < 2)
  {
    free(a[i]);
    free(a[j]);
    a[i] = 0;
    a[j] = 0;
  }
  free(a[0]);
  free(a[1]);
}
""",
}

OWNED_BAD = {
    # an index that is neither a variable nor one plus a constant: the array
    # is not followed, and that is said
    "computed_index": ("""\
void f(int i)
{
  char * owned maybe a[4] = undefined;
  a[i * 2] = malloc(4);
}
""", 4, "'a' is reached in a way the lifetime check does not follow: what it owns is not checked"),
    # a[i] and a[j] may be the same element (strict lifetimes)
    "two_index_variables": ("""\
void f(int i, int j)
{
  char * owned maybe a[2] = undefined;
  a[0] = malloc(4);
  a[1] = malloc(4);
  free(a[i]);
  free(a[j]);
}
""", 7, "'a[j]' may be freed twice"),
    # the guard said i != j, but j changed since
    "index_written_after_the_guard": ("""\
void f(int i, int j, int k)
{
  char * owned maybe a[2] = undefined;
  a[0] = malloc(4);
  a[1] = malloc(4);
  if (i == j)
  {
    free(a[0]);
    free(a[1]);
    return;
  }
  j = k;
  free(a[i]);
  free(a[j]);
}
""", 14, "'a[j]' may be freed twice"),
    # i != j on one way only
    "guard_on_one_path": ("""\
void f(int i, int j, int c)
{
  char * owned maybe a[2] = undefined;
  a[0] = malloc(4);
  a[1] = malloc(4);
  if (c && i == j)
  {
    free(a[0]);
    free(a[1]);
    return;
  }
  free(a[i]);
  free(a[j]);
}
""", 13, "'a[j]' may be freed twice"),
    "no_free_loop": ("""\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
}
""", 7, "'a[i]' leaks: it may still own memory at the end of its scope"),
    "freed_on_one_path": ("""\
void f(int c)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  if (c)
    for (i = 0; i < 4; i++)
      free(a[i]);
}
""", 10, "'a[i]' leaks: it may still own memory at the end of its scope"),
    "used_after_free": ("""\
void f(int i)
{
  char * owned maybe a[4] = undefined;
  a[i] = malloc(4);
  free(a[i]);
  use(a[i]);
}
""", 6, "'a[i]' is used after it was freed"),
    "freed_twice_by_loops": ("""\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    free(a[i]);
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""", 10, "'a[i]' is freed twice"),
    "element_after_free_loop": ("""\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    free(a[i]);
  use(a[1]);
}
""", 9, "'a[1]' is used after it was freed"),
    "maybe_untested": ("""\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    use(a[i]);
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""", 8, "'a[i]' may be null, and is passed as argument 1 of 'use', which is not maybe"),
    "stored_unfreed": ("""\
void f(int i)
{
  char * owned maybe a[4] = undefined;
  a[i] = malloc(4);
}
""", 5, "'a[i]' leaks: it may still own memory at the end of its scope"),
    "assigned_again_by_loop": ("""\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    a[i] = malloc(8);
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""", 8, "'a[i]' leaks: it may still own memory when it is assigned again"),
    "handles_not_closed": ("""\
void f(void)
{
  int owned(close) maybe fds[2] = undefined;
  for (int i = 0; i < 2; i++)
    fds[i] = open("x", 0);
}
""", 6, "'fds[i]' leaks: it may still own a handle at the end of its scope"),
    "struct_member_not_freed": ("""\
void f(void)
{
  struct box b[4] = undefined;
  for (int i = 0; i < 4; i++)
    b[i].buf = malloc(4);
}
""", 6, "'b[i].buf' leaks: it may still own memory at the end of its scope"),
    # an index plus a constant
    "shifted_used_after_free": ("""\
void f(int i)
{
  char * owned maybe a[4] = undefined;
  a[i + 1] = malloc(4);
  free(a[i + 1]);
  use(a[i + 1]);
}
""", 6, "'a[i + 1]' is used after it was freed"),
    "shifted_alloc_loop_leaks": ("""\
void f(void)
{
  char * owned maybe a[5] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i + 1] = malloc(4);
}
""", 7, "'a[i + 1]' leaks: it may still own memory at the end of its scope"),
    "shifted_freed_twice": ("""\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = -1; i < 3; i++)
    free(a[i + 1]);
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""", 10, "'a[i]' is freed twice"),
    "shifted_used_after_free_loop": ("""\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    free(a[i]);
  for (i = 0; i < 3; i++)
    use(a[i + 1]);
}
""", 10, "'a[i + 1]' is used after it was freed"),
    "shifted_member_leaks": ("""\
void f(void)
{
  struct box b[3] = undefined;
  for (int i = 1; i < 4; i++)
    b[i - 1].buf = malloc(4);
}
""", 6, "'b[i - 1].buf' leaks: it may still own memory at the end of its scope"),
    # a constant element written before the loop keeps its own state
    "constant_store_then_alloc_loop": ("""\
void f(void)
{
  char * owned maybe a[5] = undefined;
  int i;
  a[4] = 0;
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
}
""", 8, "'a[i]' leaks: it may still own memory at the end of its scope"),
    "constant_alloc_not_freed_after_loops": ("""\
void f(void)
{
  char * owned maybe a[5] = undefined;
  int i;
  a[4] = malloc(3);
  for (i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (i = 0; i < 4; i++)
    free(a[i]);
}
""", 10, "'a[4]' leaks: it still owns memory at the end of its scope"),
    # a second index variable: the elements the loops left are exact
    "second_index_partial_free": ("""\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i, j;
  for (i = 0; i < 4; i++)
    a[i] = malloc(8);
  for (j = 0; j < 3; j++)
    free(a[j]);
}
""", 9, "'a[3]' leaks: it still owns memory at the end of its scope"),
    "second_for_i_partial_free": ("""\
void f(void)
{
  char * owned maybe a[4] = undefined;
  for (int i = 0; i < 4; i++)
    a[i] = malloc(8);
  for (int i = 1; i < 4; i++)
    free(a[i]);
}
""", 8, "'a[0]' leaks: it still owns memory at the end of its scope"),
    "second_index_no_free": ("""\
void f(void)
{
  char * owned maybe a[4] = undefined;
  int i, j;
  for (i = 0; i < 4; i++)
    a[i] = malloc(8);
  for (j = 0; j < 4; j++)
    if (a[j])
      use(a[j]);
}
""", 10, "'a[0]' leaks: it still owns memory at the end of its scope"),
}


@pytest.mark.parametrize("name", sorted(OWNED_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_owned_clean(tmp_path, name, opt):
    assert _diags(tmp_path, OWNED_CLEAN[name], opt) == []


@pytest.mark.parametrize("name", sorted(OWNED_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_owned_reported(tmp_path, name, opt):
    body, line, msg = OWNED_BAD[name]
    assert _diags(tmp_path, body, opt) == [(line, msg)]


INLINE = """\
static inline void fill4(void)
{
  char * owned maybe a[4] = undefined;
  for (int i = 0; i < 4; i++)
    a[i] = malloc(4);
}
void f(void)
{
  fill4();
  fill4();
}
"""


@pytest.mark.parametrize("opt", LEVELS)
def test_inlined_said_once(tmp_path, opt):
    """An inlined body's arrays are its callee's to report: once, at every -O."""
    assert _diags(tmp_path, INLINE, opt) == [(6, "'a[i]' leaks: it may still own memory at the end of its scope")]


def test_no_lifetime_no_report(tmp_path):
    body, _, _ = OWNED_BAD["no_free_loop"]
    proc = _compile(tmp_path, body, "-Wno-lifetime")
    assert proc.returncode == 0 and "warning" not in proc.stderr


def test_werror(tmp_path):
    body, _, _ = OWNED_BAD["no_free_loop"]
    proc = _compile(tmp_path, body, "-Werror=lifetime")
    assert proc.returncode != 0 and "leaks" in proc.stderr
    proc = _compile(tmp_path, body, "-Werror", "-Wno-error=lifetime")
    assert proc.returncode == 0 and "leaks" in proc.stderr


@pytest.mark.parametrize("name", ["alloc_then_free_loops", "goto_cleanup", "member_array"])
def test_objects_unchanged(tmp_path, name):
    """The check never changes code."""
    for opt in ("-O1", "-O2", "-Os"):
        with_ = _compile(tmp_path, OWNED_CLEAN[name], opt, obj="a.o")
        src = tmp_path / "t.c"
        without = subprocess.run(
            [str(TCC), "-ccompat", "-Wno-lifetime", "-c", opt, str(src), "-o", str(tmp_path / "b.o")],
            capture_output=True,
            text=True,
        )
        assert with_.returncode == 0 and without.returncode == 0
        assert (tmp_path / "a.o").read_bytes() == (tmp_path / "b.o").read_bytes()
