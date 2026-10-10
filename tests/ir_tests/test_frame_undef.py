"""Copies of undefined values are not emitted (source/ir/frame_dfe.c).

The kernel is Zig built through its C backend.  Every std.log call builds an
array of tagged values; each one is a temporary whose payload union is wider
than the member written, moved through two more temporaries into the array:

    t10.tag = 0; t10.payload.u = x; t13 = t10; t8 = t13; arr[i] = t8;

The union's tail is never written.  tcc used to keep the copy: a store of a
vreg nothing defines (a garbage register, which also costs a callee-saved
push) or a load of a frame slot nothing stores to, then the store of that.
On the kernel (interface_gen_vcall MmcFile.read, 8 slots of 8 bytes and a
store each) clang writes only the defined words.

Each function here is bounded by the number of stores it needs.  Runtime
correctness of the same shapes (a word written on one path only, through a
pointer, by a callee, volatile): 883_frame_undef_copy.c.
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
typedef unsigned long long u64;
typedef struct { unsigned char tag;
                 union { u64 u; double f; struct { const char *p; unsigned n; } s; unsigned w[4]; } payload; } Value;
struct Slice { const Value *p; unsigned n; };
extern void emit(const char *fmt, struct Slice args);

static Value make_u(unsigned x)
{
  Value t10, t13;
  t10.tag = 0;
  t10.payload.u = x;
  t13 = t10;
  return t13;
}

/* Two elements: tag, payload.u (two words) each = 6 stores into the array,
 * plus the slice's two words handed over in the argument area. */
void two_values(unsigned a, unsigned b)
{
  Value arr[2];
  Value t8;
  t8 = make_u(a);
  arr[0] = t8;
  t8 = make_u(b);
  arr[1] = t8;
  struct Slice s = {arr, 2};
  emit("a=%u b=%u", s);
}

/* The same in a loop body. */
void in_loop(unsigned n)
{
  for (unsigned i = 0; i < n; i++)
  {
    Value arr[1];
    Value t8 = make_u(i * 7u + 1u);
    arr[0] = t8;
    struct Slice s = {arr, 1};
    emit("i=%u", s);
  }
}
"""

# function -> (most stores to the frame it may need, most registers it may push)
BOUNDS = {
    "two_values": (8, 5),
    "in_loop": (5, 6),
}


def _disasm(tmp_path, opt):
    src = tmp_path / "u.c"
    obj = tmp_path / "u.o"
    src.write_text(SOURCE)
    subprocess.run([str(TCC), opt, "-mfloat-abi=hard", "-c", str(src), "-o", str(obj)], check=True)
    return subprocess.run([OBJDUMP, "-d", str(obj)], capture_output=True, text=True, check=False).stdout


def _stats(text):
    """function -> (stores addressed off sp, registers pushed)."""
    res, cur = {}, None
    for line in text.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            cur = m.group(1)
            res[cur] = [0, 0]
            continue
        m = re.match(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{4} ?)+\s+(\S+)\s*(.*)$", line)
        if not m or cur is None:
            continue
        op, args = m.group(1), m.group(2)
        if re.match(r"str(d|b|h)?(\.w)?$", op) and "[sp" in args:
            res[cur][0] += 2 if op.startswith("strd") else 1
        elif op.startswith(("push", "stmdb")) and "sp!" in args or op.startswith("push"):
            res[cur][1] += len(re.findall(r"\b(?:r\d+|sl|fp|ip|lr)\b", args.split("{", 1)[-1]))
    return {k: tuple(v) for k, v in res.items()}


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
@pytest.mark.parametrize("func", sorted(BOUNDS))
def test_undefined_copies_are_not_stored(tmp_path, opt, func):
    stats = _stats(_disasm(tmp_path, opt))
    assert func in stats, f"{func} not in the object: {sorted(stats)}"
    stores, pushed = stats[func]
    max_stores, max_pushed = BOUNDS[func]
    assert stores <= max_stores, f"{func} at {opt}: {stores} frame stores, at most {max_stores} are defined"
    assert pushed <= max_pushed, f"{func} at {opt}: pushes {pushed} registers (garbage sources?), at most {max_pushed}"
