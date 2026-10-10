"""Stack frames of compiled code: shapes that used to blow them up.

The yasos kernel is Zig built through its C backend, then tcc -O2.  Syscalls
run on the calling process's stack, so the kernel's frames limit every user
program; `sh -c "echo x | cat"` overflowed it (STKOF in RankedMutex.lock).
Two causes, each pinned here by a frame-size bound:

  - Zig passes `self: Self` by value: `t19 = *t14; f(t19)`, and f copies its
    parameter again.  ssa:copy_fwd reads the fields from the source and drops
    the copies (RankedMutex.lock: three 584-byte copies, 1952 bytes of frame).
  - Frame relayout -- dropping dead objects, sharing bytes between disjoint
    ones -- skipped every function with inline asm, and Zig emits asm for
    mrs/msr/barriers everywhere (irq_systick: 2184 bytes for ~30 live).

Runtime correctness of the same shapes: 614_frame_relayout_asm.c and
615_struct_copy_forward.c.  The relayout-with-asm half has no bound here: its
dead objects are the u64 return slots Zig's inlined zig.h helpers leave
behind, a shape small hand-written C does not reproduce (the front end reuses
those bytes itself); it was measured on the kernel (irq_systick 2184 -> 72,
hard_fault_main 9056 -> 704 bytes) and 614 guards its correctness.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
OBJDUMP = shutil.which("arm-none-eabi-objdump")

pytestmark = pytest.mark.skipif(
    OBJDUMP is None or not TCC.exists(),
    reason="needs armv8m-tcc and arm-none-eabi-objdump",
)

SOURCE = r"""
struct big { int a[76]; const void *waiting_for; int b[69]; };

extern struct big *cur(void);
extern void trig(void);
extern int noinl(struct big b, const void *k);
extern void use(void *p);

/* Zig's `self: Self` method, inlined into its caller. */
static int is_blocked_on(struct big a0, const void *a1)
{
  struct big t0;
  const struct big *t1;
  t0 = a0;
  t1 = &t0;
  return t1->waiting_for != 0 && t1->waiting_for == a1;
}

void lock_shape(void *key)
{
  struct big t19;
  struct big *t14 = cur();
again:
  t19 = *t14;
  if (is_blocked_on(t19, key)) {
    trig();
    goto again;
  }
}

/* The copy passed on by value: the argument area is all it needs. */
int byval_copy(struct big *p, void *k)
{
  struct big t;
  t = *p;
  return noinl(t, k);
}

int byval_direct(struct big *p, void *k)
{
  return noinl(*p, k);
}

int two_copies(struct big *p)
{
  struct big t, u;
  t = *p;
  u = t;
  return u.a[2] * 10 + t.b[3];
}

extern void use_int(int v);

/* Escapes are events: the source's address goes out only after the last read
 * of the copy, so the call before cannot have changed it. */
int src_escapes_later(int x)
{
  struct big a, b;
  a.a[3] = x;
  a.a[4] = x + 1;
  b = a;
  use_int(b.a[3]);
  int r = b.a[4];
  use(&a);
  return r;
}

/* A reused local (Zig's C backend reuses them by type): its first live range
 * never escapes, its second one does. */
int reused_dst_escapes_later(struct big *p, int x)
{
  struct big a, t;
  a.a[3] = x;
  a.a[4] = x + 1;
  t = a;
  use_int(t.a[3]);
  int r = t.a[4];
  t = *p;
  use(&t);
  return r;
}

/* A copy of a constant (Zig's `static const __anon_N`): read where it is. */
static const struct big k_big = {{1, 2, 3}, 0, {4, 5, 6}};

int const_source(void)
{
  struct big t;
  t = k_big;
  use_int(t.a[1]);
  return t.b[2];
}

/* FatFs.get: a 296-byte optional filled from a call's result or an image,
 * read for its tag and one byte of its payload (frame_dfe.c). */
struct info { unsigned long long size; unsigned short date, time; unsigned char attr, kind;
              char name[256]; char alt[13]; };
struct opt_info { struct info payload; unsigned char is_null; };
struct eu_info { struct info payload; unsigned short error; };
extern struct eu_info stat_ext(int which);
extern const struct info undef_info_ext;

int kind_only(int which)
{
  struct opt_info t14, t17;
  struct eu_info t15 = stat_ext(which);
  if (t15.error == 0)
  {
    struct info t16 = t15.payload;
    t17.is_null = 0;
    t17.payload = t16;
    t14 = t17;
  }
  else
  {
    t14.is_null = 1;
    t14.payload = undef_info_ext;
  }
  if (!t14.is_null)
  {
    struct info t18 = t14.payload;
    return t18.kind;
  }
  return -1;
}

/* fatfs.File.open: `error.X` results whose payload is a 0xaa image built in
 * a temporary and copied in (frame_dfe.c image forwarding). */
struct named { int f[40]; char name[64]; };
struct eun { struct named payload; unsigned short error; };
extern int bump(int);

struct eun open_zig(int fail, int v)
{
  struct eun t13;
  int k = bump(v);
  if (fail)
  {
    t13.payload = (struct named){{0xaa, 0xaa, 0xaa, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, "\252\252\252\252"};
    t13.error = (unsigned short)(fail + k);
    return t13;
  }
  t13.payload = (struct named){{1, 2, 3}, "x"};
  t13.payload.f[5] = k;
  t13.error = 0;
  return t13;
}
"""


def _frames(obj):
    """Stack bytes per function: every push plus every `sub sp, #n`."""
    out = subprocess.run([OBJDUMP, "-d", str(obj)], capture_output=True, text=True, check=True).stdout
    frames, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            cur = m.group(1)
            frames[cur] = 0
            continue
        m = re.match(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{4} ?)+\s+(\S+)\s*(.*)$", line)
        if not m or cur is None:
            continue
        mn, ops = m.group(1), m.group(2)
        if mn.startswith("push") or (mn.startswith("stmdb") and ops.startswith("sp!")):
            frames[cur] += 4 * len(re.findall(r"\b(?:r\d+|lr|fp|sl|ip|sb)\b", ops.split("{")[1]))
        elif mn.startswith("sub") and ops.startswith("sp,"):
            imm = re.search(r"#(\d+)", ops)
            if imm:
                frames[cur] += int(imm.group(1))
    return frames


@pytest.fixture(scope="module")
def frames(tmp_path_factory):
    d = tmp_path_factory.mktemp("frames")
    src = d / "frames.c"
    src.write_text(SOURCE)
    obj = d / "frames.o"
    subprocess.run([str(TCC), "-O2", "-c", str(src), "-o", str(obj)], check=True)
    return _frames(obj)


def test_inlined_self_by_value_has_no_copies(frames):
    # Three 584-byte copies before; the field is read straight off t14.
    assert frames["lock_shape"] <= 32, frames


def test_byval_from_copy_needs_only_the_argument_area(frames):
    assert frames["byval_copy"] <= frames["byval_direct"], frames


def test_field_read_through_two_copies(frames):
    assert frames["two_copies"] <= 16, frames



# ---------------------------------------------------------------------------
# Struct returns built in the caller's buffer (opt/flat/memory/sret_nrvo.c).
# Zig returns error unions by value: `t15 = f(); t13.payload = t15;
# t13.error = 0; return t13;` kept t13, t15 and every literal copied into
# t13 on the frame (fatfs_stat: 1272 bytes, 688 after).  The caller's buffer
# is reachable by the callee only through the hidden pointer (tcc's callers
# make sure of it), so all of them live in it, across calls.

SRET_SOURCE = r"""
struct info { unsigned long long size; unsigned short date, time; unsigned char kind, attr; char name[256]; char alt[13]; };
struct eu_info { struct info payload; unsigned short error; };
struct filinfo { unsigned long fsize; unsigned short fdate, ftime; unsigned char fattrib; char altname[13]; char fname[256]; };

extern unsigned f_stat(const char *path, struct filinfo *fi);
extern struct info from_filinfo(struct filinfo fi);
extern void note(unsigned code);

struct eu_info stat_shape(const char *path)
{
  struct eu_info t13;
  struct info t15;
  struct filinfo t2;
  unsigned r = f_stat(path, &t2);
  if (r) {
    note(r);
    t13.payload = (struct info){0xaaaaaaaaaaaaaaaaull, 0xaaaa, 0xaaaa, 0, 0xaa, {0}, {0}};
    t13.error = (unsigned short)r;
    return t13;
  }
  t15 = from_filinfo(t2);
  t13.payload = t15;
  t13.error = 0;
  return t13;
}

extern struct info make_info(unsigned k);
extern unsigned peek_info(const struct info *i);

/* built across calls, its address handed to one */
struct info across_calls(unsigned k)
{
  struct info r = make_info(k);
  r.kind = 3;
  r.attr = (unsigned char)peek_info(&r);
  note(r.kind);
  return r;
}
"""


@pytest.fixture(scope="module")
def sret_frames(tmp_path_factory):
    d = tmp_path_factory.mktemp("sret_frames")
    src = d / "sret.c"
    src.write_text(SRET_SOURCE)
    obj = d / "sret.o"
    subprocess.run([str(TCC), "-O2", "-c", str(src), "-o", str(obj)], check=True)
    return _frames(obj)


def test_error_union_built_in_callers_buffer(sret_frames):
    # Was t13 + t15 + the literal (~900 bytes) besides the FILINFO the call
    # fills and the by-value argument area.
    assert sret_frames["stat_shape"] <= 640, sret_frames


def test_result_built_across_calls(sret_frames):
    # Was the 288-byte local plus the call's own result buffer.
    assert sret_frames["across_calls"] <= 32, sret_frames


def test_merge_analysis_stays_linear(tmp_path):
    # Each local copied into the returned object is a merge candidate whose
    # check walks the function; looked up per access and per candidate
    # without bound, 1200 of them took 123 s to compile (0.4 s before the
    # merge existed).  The candidates are capped and the lookups cached.
    import time

    lines = [
        "typedef struct { long v[10]; } P;",
        "typedef struct { P payload; unsigned short err; } EU;",
        "extern P mk(int); extern void note(long); extern int cond(int);",
        "EU big(int k) {",
        "  EU r; r.err = 0;",
    ]
    for i in range(1200):
        lines.append(
            f"  P t{i} = mk(k + {i}); long *q{i} = &t{i}.v[{i % 10}]; *q{i} += k;"
            f" note(*q{i}); if (cond({i})) r.payload = t{i};"
        )
    lines.append("  return r;\n}")
    src = tmp_path / "many_copies.c"
    src.write_text("\n".join(lines) + "\n")
    start = time.monotonic()
    subprocess.run([str(TCC), "-O2", "-c", str(src), "-o", str(tmp_path / "m.o")], check=True)
    assert time.monotonic() - start < 15
def test_copy_from_source_escaping_later(frames):
    # 1168 bytes when any escape anywhere blocked the copy: one object now.
    assert frames["src_escapes_later"] <= 600, frames


def test_copy_into_reused_local_escaping_later(frames):
    assert frames["reused_dst_escapes_later"] <= 600, frames


def test_copy_of_read_only_data_reads_it(frames):
    # 584 bytes of copy before; a call between cannot change .rodata.
    assert frames["const_source"] <= 16, frames
def test_optional_read_for_its_tag_keeps_only_the_read_bytes(frames):
    # The call's 296-byte result buffer stays (the callee writes it all); the
    # two copies of the optional keep 24 bytes each instead of 304.
    assert frames["kind_only"] <= 360, frames


def test_error_image_built_in_the_result(frames):
    # The 228-byte literal temporary is gone: only the result object is left.
    assert frames["open_zig"] <= 260, frames


# ---------------------------------------------------------------------------
# A struct result's buffer is handed to the callee as is when nothing else can
# reach it (tcc_ir_sret_dealias); every other one gets a fresh temporary and a
# copy.  tests2/119 returns a 256 KB `struct { char a[262144]; }`: its
# temporary sat at an odd offset (alignment 1, and the caller keeps only
# word-aligned buffers), and a computed goto in the function rerouted every
# call -- either way 256 KB more frame, past the board's 1 MB stack.

CHAR_RESULT_SOURCE = r"""
struct cb { char a[1001]; };
extern void fill(struct cb *p);
extern struct cb pass(struct cb c);
extern void take(char *p);

void char_result(void)
{
  char c[3];
  struct cb b;
  fill(&b);
  take(c);
  pass(b);
}

void char_result_computed_goto(int k)
{
  static void *t[] = { &&x, &&y };
  struct cb b;
  fill(&b);
  goto *t[k & 1];
x:
  pass(b);
y:;
}
"""


@pytest.fixture(scope="module")
def char_result_frames(tmp_path_factory):
    d = tmp_path_factory.mktemp("char_result")
    src = d / "char_result.c"
    src.write_text(CHAR_RESULT_SOURCE)
    obj = d / "char_result.o"
    subprocess.run([str(TCC), "-O2", "-c", str(src), "-o", str(obj)], check=True)
    return _frames(obj)


def test_char_struct_result_needs_no_second_buffer(char_result_frames):
    # b, the by-value argument and the result: 3 x 1001 (4 with the copy).
    assert char_result_frames["char_result"] <= 3100, char_result_frames


def test_computed_goto_keeps_the_result_buffer(char_result_frames):
    # b and the by-value argument: the result's buffer shares their bytes
    # (3032 when the goto made the call reroute).
    assert char_result_frames["char_result_computed_goto"] <= 2100, char_result_frames
