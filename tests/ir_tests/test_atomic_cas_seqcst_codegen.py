"""Atomic memory orders on ARMv8-M and the inline compare-exchange's shape.

The C11 mapping for ARMv8 (gcc, LLVM): acquire and seq_cst loads are LDA,
release and seq_cst stores STL, a read-modify-write LDAEX/STLEX by its order,
with no DMB anywhere.  ARMv7-M has no LDA/STL and keeps the DMB mapping.  An
-minline-atomics compare-exchange is `ldrex; cmp; bne out; strex; cmp; bne
top` -- no IT block -- and closes a failed reservation (CLREX) on the
failure path only, never after a successful store.
"""
import os
import re
import subprocess

import pytest

from test_codegen_asm import OBJDUMP, ROOT, _compile

TYPES = ("u8", "s8", "u16", "s16", "u32", "s32")
FIXTURE = "bug_atomic_cas_seqcst_orders"


def _obj(flags):
    """The fixture's object for `flags`, private to this pytest-xdist worker.

    _compile names the object after the source and the flags only, and every
    worker builds the module-scoped fixtures itself: workers sharing one .o
    rewrite it while another runs objdump on it.  An unused -D naming the
    worker gives each worker its own object."""
    worker = os.environ.get("PYTEST_XDIST_WORKER", "main")
    return _compile(FIXTURE, [*flags, f"-DTEST_XDIST_WORKER_{worker}"], ROOT / "tests/ir_tests")


def _funcs(obj):
    """{func: [(addr, mnemonic, operands)]} from objdump -d."""
    out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", str(obj)], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, errors="replace", check=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        h = re.match(r"^\s*([0-9a-f]+)\s+<([^>]+)>:$", line)
        if h:
            cur = funcs.setdefault(h.group(2), [])
            continue
        i = re.match(r"^\s*([0-9a-f]+):\s+(\S+)(?:\s+(.*))?$", line)
        if i and cur is not None and not i.group(2).startswith("."):
            cur.append((int(i.group(1), 16), i.group(2), i.group(3) or ""))
    return funcs


def _text(insns):
    return "\n".join(f"{a:4x}: {m} {o}" for a, m, o in insns)


def _count(insns, rx):
    r = re.compile(rx)
    return sum(1 for _, m, _ in insns if r.match(m))


def _cas_loops(insns):
    """(top, back) index pairs: an exclusive load and the branch back to it."""
    loops = []
    for k, (addr, m, _) in enumerate(insns):
        if not re.match(r"^(ldrex|ldaex)", m):
            continue
        for j in range(k + 1, len(insns)):
            a, mj, oj = insns[j]
            t = re.match(r"^\s*([0-9a-f]+)", oj)
            if mj.startswith("b") and t and int(t.group(1), 16) == addr:
                loops.append((k, j))
                break
    return loops


def _target(o):
    """A branch's target address, from objdump's operands (`r3, 2a <f+0x..>`)."""
    t = re.match(r"^\s*(?:r\d+,\s*)?([0-9a-f]+)\s", o)
    return int(t.group(1), 16) if t else None


def _assert_cas_shape(insns, name):
    text = _text(insns)
    loops = _cas_loops(insns)
    assert loops, f"{name}: no exclusive loop:\n{text}"
    at = {a: m for a, m, _ in insns}
    for top, back in loops:
        body = insns[top:back + 1]
        assert _count(body, r"^clrex") == 0, f"{name}: CLREX inside the loop:\n{text}"
        assert _count(body, r"^it") == 0, f"{name}: IT block in the loop:\n{text}"
        lo, hi = body[0][0], body[-1][0]
        out = insns[back + 1][0] if back + 1 < len(insns) else None
        stores = [k for k, (_, m, _) in enumerate(body) if re.match(r"^(strex|stlex)", m)]
        assert len(stores) == 1, f"{name}: not one exclusive store:\n{text}"
        s = stores[0]
        # The mismatch: a branch before the store that leaves the loop (to
        # right after it, or to the code the CAS's result selects).
        for _, m, o in body[1:s]:
            if m.startswith(("b", "cb")):
                t = _target(o)
                assert t is not None and not lo <= t <= hi, f"{name}: mismatch stays in the loop:\n{text}"
        # The store's status: `cmp; bne top` with success falling out, or
        # `cmp; beq <success>; b top` (`cbz status, <success>; b top`) when
        # success branches to its code.
        tail = [m for _, m, _ in body[s + 1:]]
        if re.match(r"^b(\.n|\.w)?$", tail[-1]) and tail[0] == "cbz":
            assert len(tail) == 2, f"{name}: store status test:\n{text}"
            success = _target(body[s + 1][2])
            assert success is not None and not lo <= success <= hi, f"{name}: success stays in the loop:\n{text}"
            # ldrex; cmp; bne; strex; cbz; b
            limit = 6
        elif re.match(r"^b(\.n|\.w)?$", tail[-1]):
            assert len(tail) == 3 and tail[0].startswith("cmp") and tail[1].startswith("beq"), \
                f"{name}: store status test:\n{text}"
            success = _target(body[s + 2][2])
            assert success is not None and not lo <= success <= hi, f"{name}: success stays in the loop:\n{text}"
            # ldrex; cmp; bne; strex; cmp; beq; b (CBNZ folds the compare with 0)
            limit = 7
        else:
            assert len(tail) == 2 and tail[0].startswith("cmp") and tail[1].startswith("bne"), \
                f"{name}: store status test:\n{text}"
            success = out
            # ldrex; cmp; bne; strex; cmp; bne
            limit = 6
        for _, m, o in body[s + 1:]:
            if m.startswith("b"):
                t = _target(o)
                assert t in (lo, success), f"{name}: loop exits elsewhere:\n{text}"
        assert len(body) <= limit, f"{name}: loop of {len(body)} instructions:\n{text}"
        # A CLREX where a successful store lands runs after every success.
        if success is not None:
            assert not at.get(success, "").startswith("clrex"), f"{name}: CLREX on the success path:\n{text}"
    assert _count(insns, r"^clrex") >= 1, f"{name}: a failed CAS leaves its reservation open:\n{text}"


def _retests(insns):
    """Compares of a CAS loop's loaded value after the loop: the result test
    the loop's exits already made."""
    n = 0
    for top, back in _cas_loops(insns):
        old = re.match(r"^\s*(r\d+|ip|lr)", insns[top][2]).group(1)
        for _, m, o in insns[back + 1:]:
            if re.match(r"^(cmp|cbz|cbnz|tst|teq)", m) and re.match(rf"^\s*{old}\b", o):
                n += 1
    return n


@pytest.fixture(scope="module", params=["-O1", "-O2", "-Os"])
def v8(request):
    return _funcs(_obj([request.param, "-minline-atomics"]))


@pytest.fixture(scope="module", params=["-O2"])
def v8_calls(request):
    return _funcs(_obj([request.param]))


@pytest.mark.parametrize("t", TYPES)
def test_v8_loads_stores(v8, t):
    f = v8
    for fn in ("ld_relaxed",):
        insns = f[f"{t}_{fn}"]
        assert _count(insns, r"^(lda|dmb)") == 0 and _count(insns, r"^ldr") == 1, _text(insns)
    for fn in ("ld_consume", "ld_acquire", "ld_seq_cst", "ld_acq_rel", "ld_with"):
        insns = f[f"{t}_{fn}"]
        assert _count(insns, r"^lda[bh]?$") == 1, f"{t}_{fn}: not one LDA:\n{_text(insns)}"
        assert _count(insns, r"^dmb") == 0, f"{t}_{fn}: DMB:\n{_text(insns)}"
    insns = f[f"{t}_st_relaxed"]
    assert _count(insns, r"^(stl|dmb)") == 0 and _count(insns, r"^str") == 1, _text(insns)
    for fn in ("st_release", "st_seq_cst", "st_with"):
        insns = f[f"{t}_{fn}"]
        assert _count(insns, r"^stl[bh]?$") == 1, f"{t}_{fn}: not one STL:\n{_text(insns)}"
        assert _count(insns, r"^dmb") == 0, f"{t}_{fn}: DMB:\n{_text(insns)}"


@pytest.mark.parametrize("t", TYPES)
def test_v8_rmw_orders(v8, t):
    f = v8
    expect = {"relaxed": ("ldrex", "strex"), "acquire": ("ldaex", "strex"), "release": ("ldrex", "stlex"),
              "acq_rel": ("ldaex", "stlex"), "seq_cst": ("ldaex", "stlex")}
    for order, (ld, st) in expect.items():
        insns = f[f"{t}_add_{order}"]
        text = _text(insns)
        assert _count(insns, rf"^{ld}[bh]?$") == 1 and _count(insns, rf"^{st}[bh]?$") == 1, f"{t} {order}:\n{text}"
        assert _count(insns, r"^dmb") == 0, f"{t}_add_{order}: DMB:\n{text}"
    insns = f[f"{t}_xchg_seq_cst"]
    assert _count(insns, r"^ldaex") == 1 and _count(insns, r"^stlex") == 1 and _count(insns, r"^dmb") == 0, _text(insns)


@pytest.mark.parametrize("t", TYPES)
def test_v8_cas_shape(v8, t):
    for fn, (ld, st) in {"cas_strong": ("ldaex", "stlex"), "cas_weak": ("ldaex", "strex"),
                         "cas_release": ("ldrex", "stlex"), "cas_relaxed": ("ldrex", "strex")}.items():
        insns = v8[f"{t}_{fn}"]
        assert _count(insns, rf"^{ld}[bh]?$") == 1 and _count(insns, rf"^{st}[bh]?$") == 1, _text(insns)
        assert _count(insns, r"^dmb") == 0, f"{t}_{fn}: DMB:\n{_text(insns)}"
        _assert_cas_shape(insns, f"{t}_{fn}")


def test_v8_cas_callers(v8):
    for fn in ("cas_old", "cas_big_const", "cas_small_const", "cas_zero", "cas_void", "spin_lock"):
        _assert_cas_shape(v8[fn], fn)
    insns = v8["spin_lock"]
    mn = [m for _, m, _ in insns]
    assert mn.index("clrex") < mn.index("wfe"), _text(insns)
    assert _count(insns, r"^it") == 0, _text(insns)
    # The lock is the loop, whose exits are the lock's own branch, and the
    # WFE path: no re-test of the loaded value after the loop.
    assert _retests(insns) == 0, _text(insns)
    assert len([m for m in mn if m != "nop"]) <= 10, _text(insns)


def test_v8_seqlock(v8):
    for fn, rx, n in (("seq_load", r"^lda$", 2), ("seq_store", r"^stl$", 2)):
        insns = v8[fn]
        assert _count(insns, rx) == n and _count(insns, r"^dmb") == 0, f"{fn}:\n{_text(insns)}"
    # The odd test on the back edge: `ands; bne`, not `and; cmp #0; bne`.
    insns = v8["seq_load"]
    assert _count(insns, r"^ands") == 1 and _count(insns, r"^and(\.w)?$") == 0, _text(insns)


def test_v8_runtime_calls_keep_inline_loads(v8_calls):
    """Without -minline-atomics the read-modify-writes call the runtime
    (full DMBs around the loop), the loads and stores stay LDA/STL."""
    f = v8_calls
    assert any("__atomic_fetch_add_4" in o for _, m, o in f["u32_add_seq_cst"] if m.startswith("b"))
    assert _count(f["u32_ld_seq_cst"], r"^lda$") == 1 and _count(f["u32_st_seq_cst"], r"^stl$") == 1


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_v7m_keeps_dmb_mapping(opt):
    """ARMv7-M has no LDA/STL/LDAEX/STLEX: DMB per order, as before."""
    f = _funcs(_obj([opt, "-minline-atomics", "-march=armv7-m"]))
    for t in TYPES:
        def dmbs(fn):
            insns = f[f"{t}_{fn}"]
            assert _count(insns, r"^(lda|stl)") == 0, f"{t}_{fn}:\n{_text(insns)}"
            return _count(insns, r"^dmb")
        assert dmbs("ld_relaxed") == 0 and dmbs("st_relaxed") == 0
        assert dmbs("ld_acquire") == 1 and dmbs("ld_seq_cst") == 1 and dmbs("ld_with") == 1
        assert dmbs("st_release") == 1 and dmbs("st_seq_cst") == 2 and dmbs("st_with") == 2
        assert dmbs("add_seq_cst") == 2 and dmbs("add_acquire") == 1 and dmbs("add_release") == 1
        assert dmbs("add_relaxed") == 0 and dmbs("cas_strong") == 2
        insns = f[f"{t}_ld_seq_cst"]
        ld = [k for k, (_, m, _) in enumerate(insns) if m.startswith("ldr")]
        dmb = [k for k, (_, m, _) in enumerate(insns) if m.startswith("dmb")]
        assert ld and dmb and ld[0] < dmb[0], _text(insns)
        insns = f[f"{t}_st_seq_cst"]
        st = [k for k, (_, m, _) in enumerate(insns) if m.startswith("str")]
        dmb = [k for k, (_, m, _) in enumerate(insns) if m.startswith("dmb")]
        assert len(st) == 1 and dmb[0] < st[0] < dmb[1], _text(insns)
