"""Volatile accesses keep their exact width, count and order.

Each function in asm/volatile_trace.c carries, in a `/*@ ... */` comment right
above it, the trace of real memory accesses it must compile to; the header of
that file documents the notation.  This compiles the file once per -O level,
disassembles it, and checks every case separately, so a failure names the
shape that broke and the level it broke at.

It complements test_codegen_asm.py's volatile tests, which assert minimum
access counts: a minimum cannot see a volatile access that is duplicated,
widened (four strb -> one str), fused (two str -> one strd) or reordered.

A case marked `bug(LEVELS): slug` is a known open miscompile described in
docs/bugs/<slug>.md, and is a strict xfail at those levels -- the fix must
delete the clause, or the XPASS fails the run.
"""

import functools
import os
import re
import subprocess
from collections import Counter
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent.parent  # libs/tinycc
TCC = ROOT / "armv8m-tcc"
CASES = Path(__file__).parent / "asm" / "volatile_trace.c"
BUILD_DIR = Path(__file__).parent / "build" / "asm"
BUGS_DIR = ROOT / "docs" / "bugs"
OBJDUMP = "arm-none-eabi-objdump"

LEVELS = ("-O0", "-O1", "-O2", "-Os")
COND = r"(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)"
SIZE_SUFFIX = {"": "4", "b": "1", "sb": "1", "h": "2", "sh": "2", "d": "8"}


# -----------------------------------------------------------------------------
# The expectations
# -----------------------------------------------------------------------------
def _parse_cases():
    """{function name: spec dict} from the `/*@ ... */` annotations."""
    text = CASES.read_text()
    cases = {}
    pat = re.compile(r"/\*@(.*?)\*/\s*(?:[\w\s\*]+?)\b(\w+)\s*\(", re.S)
    for m in pat.finditer(text):
        body, name = m.group(1), m.group(2)
        spec = {"expect": None, "per_level": {}, "each": None, "unordered": False,
                "stack": False, "loadwords": None, "bug": {}}
        for clause in (c.strip() for c in body.split(";")):
            if not clause:
                continue
            key, _, val = clause.partition(":")
            key, val = key.strip(), val.strip()
            if key == "expect":
                spec["expect"] = [alt.split() for alt in val.split("|")]
            elif key in ("O0", "O1", "O2", "Os"):
                spec["per_level"]["-" + key] = [alt.split() for alt in val.split("|")]
            elif key == "each":
                spec["each"] = set(val.split())
            elif key == "loadwords>=":
                spec["loadwords"] = int(val)
            elif key == "unordered":
                spec["unordered"] = True
            elif key == "stack":
                spec["stack"] = True
            elif key.startswith("bug("):
                for lvl in key[4:-1].split():
                    spec["bug"]["-" + lvl] = val
            else:
                raise ValueError(f"{name}: unknown clause {clause!r}")
        assert name not in cases, f"duplicate case {name}"
        cases[name] = spec
    return cases


CASE_SPECS = _parse_cases()


# -----------------------------------------------------------------------------
# The traces
# -----------------------------------------------------------------------------
def _token(mnem, ops):
    """Trace token for one instruction, or None if it is not a memory access.

    Literal-pool loads (`[pc, #N]`) materialize a constant, not an access.
    Stack accesses get an `s` prefix so a case can leave them out."""
    m = re.match(r"^(ldr|str|ldm|stm|vldr|vstr)(\w*)(?:\.[nw])?$", mnem)
    if not m or "[pc" in ops:
        return None
    base, suf = m.groups()
    kind = "L" if base[0] == "l" or base == "vldr" else "S"
    if base in ("ldm", "stm"):
        regs = re.search(r"\{([^}]*)\}", ops)
        n = len(regs.group(1).split(",")) if regs else 0
        return ("s" if ops.startswith("sp") else "") + f"{kind}m{n}"
    if base in ("vldr", "vstr"):
        width = "8" if re.match(r"d\d+", ops) else "4"
    else:
        suf = re.sub(COND + "$", "", suf) if suf not in SIZE_SUFFIX else suf
        if suf not in SIZE_SUFFIX:
            return None  # ldrex/strex/ldaex...: not produced for plain volatile
        width = SIZE_SUFFIX[suf]
    mem = re.search(r"\[([^\]]*)\]", ops)
    if not mem:
        return None
    parts = [x.strip() for x in mem.group(1).split(",")]
    off = "@0" if len(parts) == 1 else ("@" + str(int(parts[1][1:].split()[0], 0)) if parts[1].startswith("#") else "@r")
    return ("s" if parts[0] == "sp" else "") + kind + width + off


@functools.lru_cache(maxsize=None)
def _traces(level):
    """{function: [token, ...]} for the case file compiled at `level`."""
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    # One object per worker: pytest-xdist workers each fill their own cache,
    # and a shared path let one disassemble another's half-written object.
    obj = BUILD_DIR / f"volatile_trace{level}.{os.getpid()}.o"
    cmd = [str(TCC), level, "-nostdlib", "-fvisibility=hidden", "-mcpu=cortex-m33", "-mthumb",
           "-mfloat-abi=soft", "-ffunction-sections", "-c", str(CASES), "-o", str(obj)]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    assert r.returncode == 0, f"compile failed: {' '.join(cmd)}\n{r.stderr}"
    r = subprocess.run([OBJDUMP, "-dr", "--no-show-raw-insn", str(obj)], capture_output=True, text=True,
                       errors="replace")
    assert r.returncode == 0, r.stderr

    funcs, cur = {}, None
    for line in r.stdout.splitlines():
        header = re.match(r"^\s*[0-9a-f]+\s+<([^>]+)>:$", line)
        if header:
            cur = funcs.setdefault(header.group(1), [])
            continue
        if cur is None:
            continue
        reloc = re.match(r"^\s*[0-9a-f]+:\s+R_ARM_THM_(?:CALL|JUMP24)\s+(\S+)", line)
        if reloc:
            cur.append("C:" + reloc.group(1))
            continue
        ins = re.match(r"^\s*[0-9a-f]+:\s+(\S+)\s*(.*)$", line)
        if ins:
            tok = _token(ins.group(1), ins.group(2).split(";")[0].split("@")[0].strip())
            if tok:
                cur.append(tok)
    return funcs


def _load_words(tok):
    if not tok.startswith("L"):
        return 0
    w = tok[1:].split("@")[0]
    return int(w[1:]) if w.startswith("m") else max(1, int(w) // 4)


def _matches(got, want, check_offsets, unordered):
    if len(got) != len(want):
        return False
    if unordered:  # offsets cannot pair up once sorted; compare kinds only
        return sorted(g.split("@")[0] for g in got) == sorted(w.split("@")[0] for w in want)
    pairs = list(zip(got, want))
    for g, w in pairs:
        if "@" in w and check_offsets:
            if g != w:
                return False
        elif g.split("@")[0] != w.split("@")[0]:
            return False
    return True


def _check(name, level):
    """None if the case compiled as specified at `level`, else a message."""
    spec = CASE_SPECS[name]
    trace = _traces(level).get(name)
    if trace is None:
        return f"{name} missing from the object at {level}"
    if spec["stack"]:
        trace = [t[1:] if t.startswith("s") else t for t in trace]
    else:
        trace = [t for t in trace if not t.startswith("s")]
    shown = " ".join(trace) or "(no accesses)"

    if spec["loadwords"] is not None:
        words = sum(_load_words(t) for t in trace)
        if words < spec["loadwords"]:
            return f"{name} at {level}: read {words} word(s) of a volatile aggregate, want >= {spec['loadwords']}: {shown}"
    if spec["each"] is not None:
        kinds = [t.split("@")[0] for t in trace]
        stray = sorted(set(kinds) - spec["each"])
        absent = sorted(spec["each"] - set(kinds))
        if stray or absent:
            return (f"{name} at {level}: want only and all of {sorted(spec['each'])}, "
                    f"stray {stray}, absent {absent}: {shown}")
    alts = spec["per_level"].get(level, spec["expect"])
    if alts is not None:
        if not any(_matches(trace, alt, level != "-O0", spec["unordered"]) for alt in alts):
            want = " | ".join(" ".join(a) or "(no accesses)" for a in alts)
            return f"{name} at {level}: want {want}, got {shown}"
    return None


# -----------------------------------------------------------------------------
# The tests
# -----------------------------------------------------------------------------
def _params():
    for name, spec in CASE_SPECS.items():
        for level in LEVELS:
            marks = []
            slug = spec["bug"].get(level)
            if slug:
                marks.append(pytest.mark.xfail(strict=True, reason=f"open bug: docs/bugs/{slug}.md"))
            yield pytest.param(name, level, id=f"{name}{level}", marks=marks)


@pytest.mark.parametrize("name,level", list(_params()))
def test_volatile_trace(name, level):
    msg = _check(name, level)
    assert msg is None, msg


def test_volatile_trace_case_file_parsed():
    """Every annotated case was found, and every one is in the object."""
    assert len(CASE_SPECS) >= 150, f"only {len(CASE_SPECS)} annotated cases parsed"
    text = CASES.read_text()
    assert text.count("/*@") == len(CASE_SPECS), "an annotation did not attach to a function"
    missing = sorted(set(CASE_SPECS) - set(_traces("-O2")))
    assert not missing, f"cases not in the object: {missing}"


def test_volatile_trace_bug_reports_exist():
    """A `bug(...)` clause names a report in docs/bugs/."""
    slugs = {s for spec in CASE_SPECS.values() for s in spec["bug"].values()}
    missing = sorted(s for s in slugs if not (BUGS_DIR / f"{s}.md").is_file())
    assert not missing, f"bug clauses without a docs/bugs report: {missing}"
