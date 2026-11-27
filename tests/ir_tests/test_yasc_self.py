"""The method self rules (docs/static_analysis_todo.md): a `struct T *view`
parameter keeps the owned members of the struct valid.

- the callee: an owned member it frees (or moves out) must get a new value,
  or null, before it returns;
- the caller: the struct is handed over with its owned members set and not
  freed; afterwards they still own memory (a maybe one may be null);
- `drop struct T *view self`: the callee frees every owned member, and the
  caller's count as freed after the call (`defer s.clear();`).

Every case is checked with the methods declared (a call) and defined in the
same file as static functions (inlined at -O1+): the result must not depend
on -O.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

PRELUDE = """\
void *?owned malloc(unsigned n);
void free(drop void *?owned p);
int use(char *p);
struct box { char *?owned buf; char *?owned aux; int n; };
"""
DECLARED = """\
void touch(inout struct box *b);
int peek(struct box *b);
void clear(drop struct box *b);
"""
# the same methods with bodies, inlined at -O1+
DEFINED = """\
static void touch(inout struct box *b) { b->n++; }
static int peek(struct box *b) { return b->n; }
static void clear(drop struct box *b) { free(b->buf); free(b->aux); }
"""
PRELUDE_LINES = PRELUDE.count("\n") + 3


def _diags(tmp_path, body, opt, methods=DECLARED):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + methods + body)
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-c", "-fmax-errors=0", "-Wno-unused-function", opt, str(src), "-o",
                           str(tmp_path / "t.o")],
                          capture_output=True, text=True)
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[2]))
    return out


# the callee's side: a view self with an owned member left freed
CALLEE_BAD = {
    "freed_not_replaced": ("void reset(inout struct box *b)\n{\n  free(b->buf);\n}\n",
                           (4, "'b->buf' is freed when 'reset' returns: 'b' still owns it -- give it a new value or null")),
    "freed_on_one_path": ("void reset(inout struct box *b, int c)\n{\n  if (c)\n    free(b->buf);\n}\n",
                          (5, "'b->buf' is freed when 'reset' returns: 'b' still owns it -- give it a new value "
                              "or null")),  # said on the exit where it is
    "moved_out": ("char *?owned take(inout struct box *b)\n{\n  return move(b->buf);\n}\n",
                  (3, "'b->buf' is moved when 'take' returns: 'b' still owns it -- give it a new value or null")),
    "drop_partial": ("void wipe(drop struct box *b)\n{\n  free(b->buf);\n}\n",
                     (4, "'b->aux' is not freed when 'wipe' returns, which drops 'b'")),
    "drop_untouched": ("void wipe(drop struct box *b)\n{\n  b->n = 0;\n  free(b->aux);\n}\n",
                       (5, "'b->buf' is not freed when 'wipe' returns, which drops 'b'")),
    "handed_freed": ("void f(inout struct box *b)\n{\n  free(b->buf);\n  touch(b);\n  b->buf = nullptr;\n}\n",
                     (4, "'b->buf' is freed when 'b' is handed to 'touch': give it a new value first")),
}

CALLEE_CLEAN = {
    "replaced": "void reset(inout struct box *b)\n{\n  char *?owned n = malloc(8);\n  free(b->buf);\n"
    "  b->buf = move(n);\n}\n",
    "nulled": "void reset(inout struct box *b)\n{\n  free(b->buf);\n  b->buf = nullptr;\n}\n",
    "moved_out_null": "void pop(inout struct box *b)\n{\n  char *?owned n = move(b->buf);\n  if (n)\n  {\n"
    "    b->buf = malloc(4);\n    free(n);\n  }\n}\n",
    "drop_all": "void wipe(drop struct box *b)\n{\n  free(b->buf);\n  free(b->aux);\n}\n",
    "drop_null_member": "void wipe(drop struct box *b)\n{\n  free(b->buf);\n  b->buf = nullptr;\n  free(b->aux);\n}\n",
    "drop_by_method": "void wipe(drop struct box *b)\n{\n  clear(b);\n}\n",
    "const_view": "int f(struct box *b)\n{\n  return peek(b) + b->n;\n}\n",
}

# the caller's side: a local struct handed to the methods
CALLER_BAD = {
    "leaks_after_view": ("void f(void)\n{\n  struct box s = {nullptr, malloc(1), 0};\n  touch(&s);\n  free(s.aux);\n}\n",
                         (6, "'s.buf' leaks: it may still own memory at the end of its scope")),
    "freed_then_handed": ("void f(void)\n{\n  struct box s = {nullptr, malloc(1), 0};\n  free(s.aux);\n  touch(&s);\n"
                          "  s.aux = malloc(1);\n  clear(&s);\n}\n",
                          (5, "'s.aux' is freed when its struct is handed to 'touch': give it a new value first")),
    "unset_handed": ("void f(void)\n{\n  struct box s;\n  s.n = 1;\n  touch(&s);\n  clear(&s);\n}\n",
                     (5, "'s.buf' is not set when its struct is handed to 'touch'")),
    "used_after_clear": ("int f(void)\n{\n  struct box s = {nullptr, malloc(1), 0};\n  clear(&s);\n  return use(s.aux);\n}\n",
                         (5, "'s.aux' is used after it was freed")),
    "cleared_twice": ("void f(void)\n{\n  struct box s = {nullptr, malloc(1), 0};\n  clear(&s);\n  clear(&s);\n}\n",
                      (5, "'s.aux' is freed twice")),
}

CALLER_CLEAN = {
    "freed_after_view": "void f(void)\n{\n  struct box s = {nullptr, malloc(1), 0};\n  touch(&s);\n  free(s.buf);\n"
    "  free(s.aux);\n}\n",
    "defer_clear": "void f(void)\n{\n  struct box s = {nullptr, malloc(1), 0};\n  defer clear(&s);\n  touch(&s);\n"
    "  peek(&s);\n}\n",
    "clear_at_end": "void f(void)\n{\n  struct box s = {nullptr, malloc(1), 0};\n  s.buf = malloc(4);\n  touch(&s);\n"
    "  clear(&s);\n}\n",
}


def _bad_cases():
    for table in (CALLEE_BAD, CALLER_BAD):
        for name, (body, want) in table.items():
            if want is not None:
                yield name, body, want


BAD = {name: (body, want) for name, body, want in _bad_cases()}


@pytest.mark.parametrize("methods", ["declared", "defined"])
@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(BAD))
def test_reported(tmp_path, name, opt, methods):
    body, want = BAD[name]
    got = _diags(tmp_path, body, opt, DECLARED if methods == "declared" else DEFINED)
    assert want in got, got


@pytest.mark.parametrize("methods", ["declared", "defined"])
@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted({**CALLEE_CLEAN, **CALLER_CLEAN}))
def test_clean(tmp_path, name, opt, methods):
    body = {**CALLEE_CLEAN, **CALLER_CLEAN}[name]
    assert _diags(tmp_path, body, opt, DECLARED if methods == "declared" else DEFINED) == []


@pytest.mark.parametrize("name", sorted(BAD))
def test_same_at_every_level(tmp_path, name):
    body, _ = BAD[name]
    for methods in (DECLARED, DEFINED):
        ref = _diags(tmp_path, body, "-O0", methods)
        for opt in LEVELS[1:]:
            assert _diags(tmp_path, body, opt, methods) == ref, (opt, methods)


def test_tutorial_example_is_clean(tmp_path):
    body = ("struct buf { char *?owned data; int len; };\n"
            "void buf_fill(inout struct buf *b);\nvoid buf_clear(drop struct buf *b);\n"
            "void f(void)\n{\n  struct buf b = {};\n  defer buf_clear(&b);\n  buf_fill(&b);\n}\n")
    for opt in LEVELS:
        assert _diags(tmp_path, body, opt) == []
