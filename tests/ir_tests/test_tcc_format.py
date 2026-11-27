"""tcc -format: C and yasc source laid out (docs/tcc_format.md).

Each case is an input and the text tcc -format must write for it; every output
must also come out unchanged when formatted again.  The tool re-lexes what it
writes and refuses a result whose tokens differ from the input's, so these
cases are about layout only.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


def fmt(text, *args, cwd=None, name=None):
    cmd = [str(TCC), "-format", *args]
    if name:
        cmd.append(f"-assume-filename={name}")
    return subprocess.run(cmd, input=text, capture_output=True, text=True, cwd=cwd)


CASES = {
    "statements_and_blocks": (
        "int f(int a){if(a)return 1;else{a++;}while(a>0)a--;return a;}\n",
        """\
int f(int a) {
  if (a)
    return 1;
  else {
    a++;
  }
  while (a > 0)
    a--;
  return a;
}
""",
    ),
    "allman_braces_attach": (
        "void f(void)\n{\n    if (x)\n    {\n        y();\n    }\n    else\n    {\n        z();\n    }\n}\n",
        """\
void f(void) {
  if (x) {
    y();
  } else {
    z();
  }
}
""",
    ),
    "switch_and_labels": (
        "void f(int c){switch(c){case 1:x();break;case 2:{y();break;}default:z();}out:return;}\n",
        """\
void f(int c) {
  switch (c) {
  case 1:
    x();
    break;
  case 2: {
    y();
    break;
  }
  default:
    z();
  }
out:
  return;
}
""",
    ),
    "do_while_and_empty_bodies": (
        "void f(void){do{x--;}while(x);for(;;);while(*p)p++;}\n",
        """\
void f(void) {
  do {
    x--;
  } while (x);
  for (;;)
    ;
  while (*p)
    p++;
}
""",
    ),
    "operators": (
        "void f(void){p=&q;v=-1;w=a- -b;z=!k;i++;--j;q=(int)-1;r=sizeof(int)*2;s->a.b=*s->p;x=y?a:b;c=a&b;d=*p**q;}\n",
        """\
void f(void) {
  p = &q;
  v = -1;
  w = a - -b;
  z = !k;
  i++;
  --j;
  q = (int)-1;
  r = sizeof(int) * 2;
  s->a.b = *s->p;
  x = y ? a : b;
  c = a & b;
  d = *p * *q;
}
""",
    ),
    "pointers_and_casts": (
        "char * f(const char * s, struct node * * pp);\nvoid g(void){node_t * p = (node_t *) q; char *t = (char * )(intptr_t) u; size_t n = a * b;}\n",
        """\
char *f(const char *s, struct node **pp);
void g(void) {
  node_t *p = (node_t *)q;
  char *t = (char *)(intptr_t)u;
  size_t n = a * b;
}
""",
    ),
    "yasc_pointer_words": (
        # the words go against their '*' (as clang-format leaves them: `*? view`)
        "char * ?borrows(s) strchr(const char *s, int c);\n"
        "void *? owned(dlclose) dlopen(const char *? path, int mode);\n"
        "long strtol(const char *s, out char * borrows(s) * ?endptr, int base);\n"
        "int main(int argc, const char * view *argv);\n"
        "char *@ (const , owned) name; char * ? @(owned, takes(p)) keep(void *?owned p);\n"
        "int f(int a [ ? 4], int drop ? owned(close) fd, char * const p);\n"
        "int g(int c) { const char *?view *v = (const char *?[]){\"a\", nullptr}; return c ? 1 : 0; }\n",
        """\
char *?borrows(s) strchr(const char *s, int c);
void *?owned(dlclose) dlopen(const char *?path, int mode);
long strtol(const char *s, out char *borrows(s) *?endptr, int base);
int main(int argc, const char *view *argv);
char *@(const, owned) name;
char *?@(owned, takes(p)) keep(void *?owned p);
int f(int a[?4], int drop ?owned(close) fd, char *const p);
int g(int c) {
  const char *?view *v = (const char *?[]){"a", nullptr};
  return c ? 1 : 0;
}
""",
    ),
    "compound_literal_of_a_tag": (
        "void f(struct tm *tm) { *tm = (struct tm){}; *tm = (struct tm){.tm_year = 1}; }\n",
        """\
void f(struct tm *tm) {
  *tm = (struct tm){};
  *tm = (struct tm){.tm_year = 1};
}
""",
    ),
    "yasc_words_are_names_in_c": (
        "void f(void){*out++=x;out[i]=v[i];in=in*2;char *view;}\n",
        """\
void f(void) {
  *out++ = x;
  out[i] = v[i];
  in = in * 2;
  char *view;
}
""",
    ),
    "yasc_slices": (
        "int sum([]const int s){[]int mid=a[4:8];[:0]char path=p[0:n:0];[][]const int rows=r;return s[ i ];}\n"
        "char *dirname(inout char path[?:0]);\n",
        """\
int sum([]const int s) {
  []int mid = a[4:8];
  [:0]char path = p[0:n:0];
  [][]const int rows = r;
  return s[i];
}
char *dirname(inout char path[?:0]);
""",
    ),
    "yasc_casts_builtins_errors": (
        "error E{A,B};\n!int parse(const char *s);\n"
        "void f(void){uint16_t p=cast<int(uint16_t)>(atoi(a));auto d=cast<ptr(struct dirent *), align>(b+o);"
        "size_t n=@min(a,b);int x=parse(s) catch -1;int y=parse(s) catch |e| {log(e);return;};defer free(p);"
        "return error.A;}\n",
        """\
error E { A, B };
!int parse(const char *s);
void f(void) {
  uint16_t p = cast<int(uint16_t)>(atoi(a));
  auto d = cast<ptr(struct dirent *), align>(b + o);
  size_t n = @min(a, b);
  int x = parse(s) catch -1;
  int y = parse(s) catch |e| {
    log(e);
    return;
  };
  defer free(p);
  return error.A;
}
""",
    ),
    "yasc_struct_methods_tagged_union": (
        "struct stack{int *owned items;int len;int push(inout struct stack *self,int v){return 0;}};\n"
        "struct value{enum vkind k;union(k){long i when(V_INT);char *view s when(V_STR);}u;};\n",
        """\
struct stack {
  int *owned items;
  int len;
  int push(inout struct stack *self, int v) {
    return 0;
  }
};
struct value {
  enum vkind k;
  union(k) {
    long i when(V_INT);
    char *view s when(V_STR);
  } u;
};
""",
    ),
    "initializers_keep_their_lines": (
        "int a[] = { 1,2,3 };\nstruct point p = { .x = 1, .y = 2 };\nstatic const char *names[] = {\n  \"a\",\n  \"b\",\n};\n",
        """\
int a[] = {1, 2, 3};
struct point p = {.x = 1, .y = 2};
static const char *names[] = {
    "a",
    "b",
};
""",
    ),
    "continuation_lines_align": (
        "void f(void){call(a,\nb);x=(a+\nb)*3;long_name=fn(one,\n        two);limit=s>0?x\n:y;}\n",
        """\
void f(void) {
  call(a,
       b);
  x = (a +
       b) * 3;
  long_name = fn(one,
                 two);
  limit = s > 0 ? x
                : y;
}
""",
    ),
    "comments": (
        "/* file */\n\n\nint x; // one\nint yy;   // two\n          // more\nvoid f(void) {\n\n  // inside\n  g();\n  /* a\n     b */\n}\n",
        """\
/* file */

int x;  // one
int yy; // two
        // more
void f(void) {
  // inside
  g();
  /* a
     b */
}
""",
    ),
    "directives_and_branches": (
        "void f(void) {\n#if A\n  if (a) {\n#else\n  if (b) {\n#endif\n    x++;\n  }\n#define M(a) ( a )\n}\n",
        """\
void f(void) {
#if A
  if (a) {
#else
  if (b) {
#endif
    x++;
  }
#define M(a) ( a )
}
""",
    ),
    "extern_c_not_indented": (
        '#ifdef __cplusplus\nextern "C" {\n#endif\n\nint f(void);\n\n#ifdef __cplusplus\n}\n#endif\n',
        '#ifdef __cplusplus\nextern "C" {\n#endif\n\nint f(void);\n\n#ifdef __cplusplus\n}\n#endif\n',
    ),
    "off_regions_and_if0": (
        "void f(void) {\n  /* tcc-format off */\n  int   keep  =  1;\n  /* tcc-format on */\n#if 0\nnot { code 'at all\n#endif\n  int  x=1;\n}\n",
        "void f(void) {\n  /* tcc-format off */\n  int   keep  =  1;\n  /* tcc-format on */\n#if 0\nnot { code 'at all\n#endif\n  int x = 1;\n}\n",
    ),
    "return_type_on_its_own_line": (
        "static int\nold(int a,\n    int b)\n{\n  return a;\n}\n",
        "static int\nold(int a,\n    int b) {\n  return a;\n}\n",
    ),
}


@pytest.mark.parametrize("name", list(CASES))
def test_layout(name):
    src, want = CASES[name]
    r = fmt(src)
    assert r.returncode == 0, r.stderr
    assert r.stdout == want
    again = fmt(r.stdout)
    assert again.returncode == 0, again.stderr
    assert again.stdout == r.stdout, "not idempotent"


SAMPLE = "void f(void)\n{\n    if (x) { y(); } else z();\n}\n"


def test_brace_styles(tmp_path):
    want = {
        "attach": "void f(void) {\n  if (x) {\n    y();\n  } else\n    z();\n}\n",
        "linux": "void f(void)\n{\n  if (x) {\n    y();\n  } else\n    z();\n}\n",
        "allman": "void f(void)\n{\n  if (x)\n  {\n    y();\n  }\n  else\n    z();\n}\n",
    }
    for style, text in want.items():
        d = tmp_path / style
        d.mkdir()
        (d / ".tcc-format").write_text(f"BraceStyle: {style}\n")
        r = fmt(SAMPLE, name=str(d / "a.c"))
        assert r.returncode == 0, r.stderr
        assert r.stdout == text, style


def test_indent_width_and_parent_lookup(tmp_path):
    (tmp_path / ".tcc-format").write_text("# the style\nIndentWidth: 4\n")
    sub = tmp_path / "deep" / "er"
    sub.mkdir(parents=True)
    f = sub / "a.c"
    f.write_text("void f(void){g();}\n")
    r = subprocess.run([str(TCC), "-format", str(f)], capture_output=True, text=True)
    assert r.stdout == "void f(void) {\n    g();\n}\n"


def test_short_case_labels(tmp_path):
    (tmp_path / ".tcc-format").write_text("AllowShortCaseLabelsOnASingleLine: true\n")
    src = 'const char *n(int e) {\n  switch (e) {\n  case 0: return "ok";\n  case 1:\n    return "bad";\n  }\n}\n'
    r = fmt(src, name=str(tmp_path / "a.c"))
    assert r.stdout == src


def test_ignore_and_disable(tmp_path):
    (tmp_path / "vendor").mkdir()
    (tmp_path / "fixtures").mkdir()
    (tmp_path / ".tcc-format-ignore").write_text("# not ours\nvendor/*.c\n")
    (tmp_path / "fixtures" / ".tcc-format").write_text("DisableFormat: true\n")
    ugly = "int  f( void ){return 0;}\n"
    for rel in ("vendor/v.c", "fixtures/x.c", "mine.c"):
        (tmp_path / rel).write_text(ugly)
    r = subprocess.run(
        [str(TCC), "-format", "-n", "vendor/v.c", "fixtures/x.c", "mine.c"],
        capture_output=True,
        text=True,
        cwd=tmp_path,
    )
    assert r.returncode == 1
    assert r.stdout.split() == ["mine.c"]
    r = subprocess.run(
        [str(TCC), "-format", "-i", "vendor/v.c", "fixtures/x.c", "mine.c"], cwd=tmp_path, capture_output=True
    )
    assert r.returncode == 0
    assert (tmp_path / "vendor/v.c").read_text() == ugly
    assert (tmp_path / "fixtures/x.c").read_text() == ugly
    assert (tmp_path / "mine.c").read_text() == "int f(void) {\n  return 0;\n}\n"
    r = subprocess.run([str(TCC), "-format", "-n", "mine.c"], cwd=tmp_path, capture_output=True, text=True)
    assert r.returncode == 0 and r.stdout == ""


def test_unformattable_input_is_passed_through():
    src = "int x; /* never closed\n"
    r = fmt(src)
    assert r.returncode == 1
    assert "unterminated comment" in r.stderr
    assert r.stdout == src


def test_crlf_kept():
    r = subprocess.run([str(TCC), "-format"], input=b"int  x;\r\nint y ;\r\n", capture_output=True)
    assert r.stdout == b"int x;\r\nint y;\r\n"


def test_compound_literal_subscript():
    src = "void f(void) {\n  assert((int[]){1, 2}[1] == 2);\n  static []const char s = x;\n}\n"
    r = fmt(src)
    assert r.stdout == src


def test_function_pointer_casts_and_declarators():
    src = (
        "extern void (*s[])(void) __attribute__((weak));\n"
        "void q(int (*view compar)(const void *, const void *));\n"
        "void f(void) {\n"
        "  void (**init)(void) = (void (**)(void))&tbl[2];\n"
        "  x = (a * b) - c;\n"
        "  z = (void (*)(int))q;\n"
        "}\n"
    )
    r = fmt(src)
    assert r.stdout == src
