"""-Wsizeof-pointer-memaccess and -Wsizeof-array-argument (both -Wall, gcc:
-Wall and on by default; so the safe default too).

-Wsizeof-pointer-memaccess: the length argument of memset / memcpy /
memmove / memcmp / strncpy / strncat / strncmp / snprintf / bzero ... (by
name, `__builtin_` too) is exactly `sizeof p` of one of its pointer arguments
(a whole variable, or the same tokens: `sizeof s->p` with s->p passed, either
in parentheses), or sizeof of that argument's pointer type; gcc's wording
("did you mean to dereference it?" / "provide an explicit length").
-Wsizeof-array-argument: sizeof of a parameter declared as an array, which
is a pointer.
"""

import pytest

from fe_check_cases import ALL_CHECKS, TCC, Case

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

# no plain pointers in yasc (the safe default): V is `view` there, and IO
# `inout` (a view parameter's target is read-only without it)
IO = "#ifdef __YASC__\n#define IO inout\n#else\n#define IO\n#endif\n"
PRE = IO + (
    "#ifdef __YASC__\n#define V view\n#else\n#define V\n#endif\n"
    "typedef unsigned size_t;\n"
    "void * V memset(void * V d, int c, size_t n);\n"
    "void * V memcpy(void * V d, const void * V s, size_t n);\n"
    "int memcmp(const void * V a, const void * V b, size_t n);\n"
    "char * V strncpy(char * V d, const char * V s, size_t n);\n"
    "int snprintf(char * V d, size_t n, const char * V f, ...);\n"
    "struct S { int a, b; };\n"
)  # 17 lines

CASES = {
    "sizeof-pointer-memaccess": Case(
        "sizeof-pointer-memaccess",
        True,
        PRE + "void f1(IO struct S * V p, IO struct S * V q, IO char * V c)\n{\n"
        "  memset(p, 0, sizeof p); /* W: argument to 'sizeof' in 'memset' call is the same expression as the "
        "destination; did you mean to dereference it? */\n"
        "  memcpy(p, q, sizeof(q)); /* W: the same pointer type 'struct S * */\n"  # 'struct S *view' in yasc
        "  (void)memcmp(p, q, sizeof p); /* W: the same expression as the first source */\n"
        "  strncpy(c, c, sizeof c); /* W: in 'strncpy' call is the same expression as the destination; did you mean "
        "to provide an explicit length? */\n"
        "  snprintf(c, sizeof c, \"x\"); /* W: in 'snprintf' call is the same expression as the destination */\n"
        "  memset(p, 0, sizeof(struct S * V)); /* W: the same pointer type */\n"
        "}\n"
        "struct H { int * V q; struct H * V n; int * V a[2]; };\n"
        "void f2(struct H * V h, struct H * V t)\n{\n"
        "  memset(h->q, 0, sizeof h->q); /* W: the same expression as the destination; did you mean to dereference */\n"
        "  memset(h->q, 0, sizeof(h->q)); /* W: the same expression as the destination */\n"
        "  memset((h->n->q), 0, sizeof h->n->q); /* W: the same expression as the destination */\n"
        "  memcpy(t->q, h->a[1], sizeof h->a[1]); /* W: the same pointer type 'int * */\n"
        "}\n",
        PRE + "void f1(struct S * V p, struct S * V q, char * V c)\n{\n  char buf[8];\n  buf[0] = 0;\n"
        "  memset(p, 0, sizeof *p);\n  memcpy(p, q, sizeof(struct S));\n  memset(buf, 0, sizeof buf);\n"
        "  strncpy(c, buf, sizeof buf);\n  memset(p, 0, 2 * sizeof p);\n}\n"
        "struct H { int * V q; int * V r; struct H * V n; };\n"
        "void f2(struct H * V h, struct H * V t)\n{\n  memset(h->q, 0, sizeof *h->q);\n"
        "  memset(h->n, 0, sizeof t->n->n[0]);\n  memset(h->q, 0, sizeof h->q[0]);\n}\n",
    ),
    "sizeof-array-argument": Case(
        "sizeof-array-argument",
        True,
        IO + "int f1(IO int arr[10], IO char buf[16])\n{\n"
        "  int r = sizeof arr; /* W: 'sizeof' on array function parameter 'arr' will return size of 'int *' */\n"
        "  r += sizeof(buf); /* W: 'sizeof' on array function parameter 'buf' will return size of 'char *' */\n"
        "  return r;\n}\n",
        "int f1(int arr[10])\n{\n  int loc[4];\n  loc[0] = 0;\n  return sizeof arr[0] + sizeof loc + sizeof(int[10]);\n}\n",
    ),
}


@pytest.mark.parametrize("check", ALL_CHECKS, ids=lambda f: f.__name__)
@pytest.mark.parametrize("name", sorted(CASES))
def test_case(tmp_path, name, check):
    check(tmp_path, CASES[name])
