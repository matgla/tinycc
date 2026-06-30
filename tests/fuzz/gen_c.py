#!/usr/bin/env python3
"""Seedable, UB-FREE random C program generator for differential fuzzing.

This is the linchpin of Tracks 2/2a/3/3a in ``docs/plan_bug_hunting.md``.  It
emits small, self-contained C programs over the integer types
``int/unsigned/char/short/long`` (plus a couple of arrays/structs and helper
functions) that compute a deterministic rolling checksum of intermediate values
and print it.  The checksum makes the program's *output* sensitive to almost any
miscompile, while the program itself is guaranteed free of undefined behaviour.

Why UB-freedom matters
-----------------------
Both differential oracles (O0/O1/O2 self-consistency, and tcc-vs-gcc) treat any
divergence in observable output as a candidate miscompile.  If a generated
program had UB, the two compilers could *legitimately* disagree and we'd report
a false positive.  Therefore every operation that can be undefined is made
defined **by construction**:

* No signed overflow: all arithmetic that can overflow (``+ - *`` and unary ``-``)
  is performed on ``unsigned`` (well-defined modular wraparound) and only cast
  back to a signed display type at the very end via the checksum, which is
  unsigned.  Signed variables are only ever *read*; they are written from masked
  unsigned values, and comparisons on them are fine.
* Shifts: shift counts are always masked into ``[0, width)`` for the operand's
  promoted width (32 for our int-sized unsigned values), and the shifted value is
  unsigned so left shifts never overflow a signed type.
* Division / modulo: every ``/`` and ``%`` is guarded as ``b ? a / b : 0`` /
  ``b ? a % b : 1`` and the dividend is unsigned, so neither divide-by-zero nor
  the ``INT_MIN / -1`` overflow can occur.
* Array indexing: every index is masked with ``& (N - 1)`` where ``N`` is a power
  of two, so indices are always in ``[0, N)``.
* No uninitialised reads: every variable, array element and struct field is
  initialised before use.
* Bounded loops: every loop has a compile-time-bounded trip count using a fresh
  unsigned counter; loop bodies cannot change the bound.
* No pointer/aliasing tricks, no UB casts, no function-pointer games.

All values that flow into the checksum are ``unsigned`` (``uint32_t`` semantics),
so the printed result is portable across compilers and optimisation levels.

Determinism
-----------
Generation is driven entirely by ``random.Random(seed)``; the same seed always
produces a byte-identical program.  See ``generate_program(seed)``.

CLI
---
    python gen_c.py --seed 123                 # print a program to stdout
    python gen_c.py --seed 123 -o out.c        # write to a file
    python gen_c.py --count 10 --out-dir d/    # write seeds 0..9 as fuzz_NN.c
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Tunable size limits (kept small so QEMU runs are fast and gcc/tcc stay happy).
# ---------------------------------------------------------------------------
MAX_GLOBAL_VARS = 6
MAX_HELPERS = 3
MAX_STMTS_PER_BLOCK = 6
MAX_EXPR_DEPTH = 4
ARRAY_SIZE = 8            # must be a power of two (index masking relies on it)
STRUCT_FIELDS = 3
MAX_LOOP_TRIP = 12       # compile-time upper bound on any loop's trip count
MAX_DTAB = 8             # max fn-pointer dispatch-table length ("fnptr" profile)
MAX_VARARGS = 8          # max anonymous int args per vsum() call ("varargs" profile; >=5 straddles r0-r3)

assert ARRAY_SIZE & (ARRAY_SIZE - 1) == 0, "ARRAY_SIZE must be a power of two"

# Signed integer display/storage types we declare.  Reads only; never the target
# of overflow-prone arithmetic (we compute in unsigned and mask before storing).
SIGNED_TYPES = ["int", "short", "char", "long"]
UNSIGNED_TYPES = ["unsigned", "unsigned short", "unsigned char", "unsigned long"]

# ---------------------------------------------------------------------------
# Generator feature PROFILES  (Axis 2 of docs/plan_fuzz_reach_expansion.md)
# ---------------------------------------------------------------------------
# The fuzzer only ever finds bugs in the slice of C it emits.  A *profile* widens
# that slice along one feature axis (floats, pointers, bitfields, ...).
#
# HARD INVARIANT — the default profile ("int") is BYTE-IDENTICAL to the historical
# stream.  Every feature below is gated on a flag in ``Gen.features`` so that when
# the flag is absent *no* rng value is drawn and *no* text is emitted.  That keeps
# the seed->bug mapping recorded in the triage tables + fuzz memories valid.
# Expand reach by adding a NEW profile; never edit the default stream.
PROFILES = {
    "int":      frozenset(),              # default — DO NOT change its stream
    "float":    frozenset({"float"}),     # adds double/float arithmetic
    "fnptr":    frozenset({"fnptr"}),     # adds an indirect-call dispatch table
    "bitfield": frozenset({"bitfield"}),  # adds unsigned bitfields + packed structs
    "switch":   frozenset({"switch"}),    # adds dense/sparse switch + forward goto
    "struct_byval": frozenset({"struct_byval"}),  # adds by-value struct/union pass+return
    "varargs":  frozenset({"varargs"}),   # adds a variadic vsum(n, ...) + call sites
    "ptr":      frozenset({"ptr"}),       # adds restricted single-level deref + aliasing
}
DEFAULT_PROFILE = "int"

# --- "float" profile tunables ---------------------------------------------
# Literals are chosen so their value is EXACT in both float and double (mantissa
# < 2**24) and finite/normal (no Inf, NaN or denormal), so every value's bit
# pattern is identical under tcc soft-float and gcc soft-float -> any divergence
# is a real codegen bug, never a legal FP disagreement.  Results are clamped to
# |x| <= 2**40 each step so loop-carried FP can never grow to Inf.
FP_TYPES = ["double", "float"]
FP_MANT_BITS = 24                          # <= float's 24-bit significand
FP_EXP_LO, FP_EXP_HI = -12, 20
MAX_FP_VARS = 4                            # results clamped to |x| <= 2**40 (0x1p40)

# --- "bitfield" profile tunables ---------------------------------------------
# UNSIGNED fields only; widths chosen so a struct's fields sum to <= 32 bits (one
# storage unit) for the non-packed shape, and so the packed shape has at least one
# field straddling a byte boundary (driving load/store_packed_bf).  Field VALUES
# are always masked to `& ((1u<<N)-1)` before store (N==32 -> 0xffffffffu, no
# 1u<<32 UB; all BF_WIDTHS are < 32 so :32 never arises anyway).
BF_WIDTHS = [1, 2, 3, 4, 5, 6, 7, 8, 11, 13]   # all < 32; unsigned -> no signed surprises
BF_MIN_FIELDS, BF_MAX_FIELDS = 3, 5

# --- "switch" profile tunables -----------------------------------------------
# DENSE switch: >=4 consecutive cases (0..K-1) -> 100% density satisfies the
# jump-table gate.  SPARSE switch: scattered labels in a power-of-two window so
# density < 50% forces the if-chain/binary-search lowering; up to 10 cases can
# cross gcase's len>8 split.  Selector is always a masked unsigned -> in-domain.
SWITCH_DENSE_MIN, SWITCH_DENSE_MAX = 4, 8
SWITCH_SPARSE_MIN, SWITCH_SPARSE_MAX = 4, 10
SWITCH_SPARSE_WINDOW = 64                  # power-of-two mask window for the sparse selector

# --- "struct_byval" profile tunables -----------------------------------------
# A small catalogue of struct shapes chosen to straddle this fork's 4-byte
# register-vs-sret return boundary (gfunc_sret: <=4B in r0, else hidden sret ptr)
# and the 8-byte even-register AAPCS rule.  All members are unsigned-family so
# every NAMED field folds straight into cs (no FP / no raw-byte reinterpret).
MAX_STRUCT_HELPERS = 3
SB_SHAPES = [
    ("SB1", [("a", "unsigned char")]),                      # 1 byte  -> reg return
    ("SB4", [("a", "unsigned")]),                           # 4 bytes -> reg return (boundary)
    ("SB5", [("a", "unsigned"), ("b", "unsigned char")]),   # 5B used (+pad) -> sret
    ("SB8", [("a", "unsigned"), ("b", "unsigned")]),        # 8 bytes -> sret + even-reg rule
]
SB_FIELDS = {name: fields for name, fields in SB_SHAPES}


def _sb_field_mask(ctype: str) -> str:
    """Mask keeping a value in range for a struct field's storage type."""
    return "0xffu" if "char" in ctype else "0xffffffffu"


def _mask_for_type(ctype: str) -> str:
    """Bit mask that keeps an unsigned value in range for a storage type.

    We compute everything as a 32-bit unsigned value then mask down to the
    storage width before assigning, so the stored value is well-defined and the
    same on tcc and gcc.
    """
    if "char" in ctype:
        return "0xff"
    if "short" in ctype:
        return "0xffff"
    # int / unsigned / long are all 32-bit on this ARM target.
    return "0xffffffff"


class Gen:
    """Holds RNG + symbol tables while building one program."""

    def __init__(self, seed: int, features=frozenset()):
        self.rng = random.Random(seed)
        self.seed = seed
        self.features = frozenset(features)   # active profile feature flags
        # Scalar unsigned variables currently in scope and known-initialised.
        # We only *read* from the signed-typed globals; all live compute vars are
        # unsigned so arithmetic never overflows a signed type.
        self.uvars: list[str] = []       # unsigned 32-bit scalars (safe to read/op)
        self.svars: list[tuple[str, str]] = []  # (name, ctype) signed globals (read only)
        self.arrays: list[str] = []      # unsigned array names (size ARRAY_SIZE)
        self.structs: list[str] = []     # struct instance names
        self.fvars: list[tuple[str, str]] = []  # (name, ctype) FP locals ("float" profile)
        # "ptr" profile: live unsigned* pointers as (ptr_name, pointee_lvalue_text).
        # DISJOINT from uvars/arrays/structs (I2: a pointer is never a value and
        # never escapes); only ever used as *p (I9: data, not address, reaches cs).
        self.pvars: "list[tuple[str, str]]" = []
        # "bitfield" profile: bitfield struct instances + the field layout of the
        # types we declared.  bfvars entries are (instance, type_name, [(field,width)]).
        self.bfvars: "list[tuple[str, str, list[tuple[str, int]]]]" = []
        self._bf_types: "list[tuple[str, list[tuple[str, int]]]]" = []  # (type_name, fields)
        self.helpers: list[str] = []     # all helper function names (unsigned->unsigned)
        # "struct_byval" profile: by-value struct helpers (name, param_shape, ret_shape)
        # and the DAG-restricted set callable from the current context (set in main()).
        self.sbhelpers: "list[tuple[str, str, str]]" = []
        self.callable_sbhelpers: "list[tuple[str, str, str]]" = []
        # Helpers that may be CALLED from the current context.  Restricted to
        # already-defined helpers while emitting a helper body so the call graph
        # is a strict DAG -> no recursion -> guaranteed termination (no stack
        # overflow / non-terminating UB).  Set to all helpers inside main().
        self.callable_helpers: list[str] = []
        # A runtime (non-constant) value that is always in scope in the current
        # context, used to perturb the RHS of comparisons so the two operands are
        # never syntactically identical (defeats -Wtautological-compare) and the
        # comparison is never constant-foldable (defeats -Wtype-limits).  Set to
        # "cs" inside main(), "lr" inside helper bodies.
        self.cmp_nonce = "cs"
        # "fnptr" profile: the indirect-call dispatch table.  dtab_name stays None
        # until the table is declared in generate_program(); the icall leaf/stmt
        # are gated on `has("fnptr") and dtab_name` so the table must actually
        # exist (>=1 helper) before any indirect call is emitted.  _icall_depth
        # guards against nested icall args (keeps each call's args shallow and
        # bounds the total number of indirect calls per program).
        self.dtab_name: "str | None" = None
        self.dtab_n: int = 0
        self._icall_depth = 0
        self._label = 0          # forward-goto label bookkeeping ("switch" profile)
        self._counter = 0

    def fresh(self, prefix: str) -> str:
        self._counter += 1
        return f"{prefix}{self._counter}"

    def fresh_label(self) -> str:
        """A function-unique label name (labels have function scope in C)."""
        self._label += 1
        return f"L{self._label}"

    def rconst(self) -> str:
        """A random unsigned 32-bit constant literal."""
        v = self.rng.randint(0, 0xFFFFFFFF)
        return f"{v}u"

    def small_const(self) -> int:
        return self.rng.randint(1, 255)

    # ----- expression generation (always yields an unsigned 32-bit value) -----

    def expr(self, depth: int) -> str:
        """Return a C expression evaluating to a well-defined unsigned value.

        Every sub-expression is unsigned, so:
          * + - * unary- wrap modulo 2^32 (defined),
          * shifts use masked counts (defined),
          * / % are guarded against zero (defined).
        """
        if depth <= 0 or self.rng.random() < 0.30:
            return self._leaf()

        kind = self.rng.choice(
            ["add", "sub", "mul", "and", "or", "xor", "shl", "shr",
             "div", "mod", "cmp", "cond", "neg", "not", "call", "leaf"]
        )
        if kind == "leaf" or (kind == "call" and not self.callable_helpers):
            return self._leaf()

        a = self.expr(depth - 1)
        if kind in ("neg", "not"):
            op = "-" if kind == "neg" else "~"
            # The operand is ORed with 0u (value-preserving) so gcc no longer
            # tracks it as a boolean, avoiding -Wbool-operation when ``a`` happens
            # to be a comparison result.  Result is still fully defined unsigned.
            return f"({op}((unsigned)({a}) | 0u))"
        if kind == "call":
            fn = self.rng.choice(self.callable_helpers)
            b = self.expr(depth - 1)
            return f"{fn}({a}, {b})"

        b = self.expr(depth - 1)
        # Avoid syntactically-identical operands (e.g. ``x ^ x`` -> 0, ``x - x``
        # -> 0).  Such self-operations are well-defined but fold to a constant,
        # which can make a downstream comparison statically decidable and emit a
        # (non-UB, -Wextra) -Wtype-limits notice.  Perturb the right operand with
        # the runtime nonce when the two sides are textually equal.
        if b == a:
            b = f"((unsigned)({b}) ^ {self.cmp_nonce})"
        if kind == "add":
            return f"((unsigned)({a}) + (unsigned)({b}))"
        if kind == "sub":
            return f"((unsigned)({a}) - (unsigned)({b}))"
        if kind == "mul":
            return f"((unsigned)({a}) * (unsigned)({b}))"
        if kind == "and":
            return f"((unsigned)({a}) & (unsigned)({b}))"
        if kind == "or":
            return f"((unsigned)({a}) | (unsigned)({b}))"
        if kind == "xor":
            return f"((unsigned)({a}) ^ (unsigned)({b}))"
        if kind == "shl":
            # Mask shift count to [0,31]; value is unsigned so no signed overflow.
            return f"((unsigned)({a}) << ((unsigned)({b}) & 31u))"
        if kind == "shr":
            return f"((unsigned)({a}) >> ((unsigned)({b}) & 31u))"
        if kind == "div":
            # Force the divisor nonzero by ORing 1: never divide-by-zero, and
            # the dividend is unsigned so no INT_MIN/-1 overflow trap.  Using an
            # OR (rather than a comparison) also avoids gcc's tautological-compare
            # diagnostics on guard expressions.
            return f"((unsigned)({a}) / ((unsigned)({b}) | 1u))"
        if kind == "mod":
            return f"((unsigned)({a}) % ((unsigned)({b}) | 1u))"
        if kind == "cmp":
            cop = self.rng.choice(["<", ">", "<=", ">=", "==", "!="])
            # XOR a runtime nonce (in scope here: "cs" in main, "lr" in helpers)
            # into the right operand.  It is value-defined and:
            #   * makes the two operands provably non-identical -> no
            #     -Wtautological-compare self-comparison (this warning is in -Wall);
            #   * keeps the comparison non-constant -> no -Wtype-limits;
            #   * is not a boolean -> no -Wbool-compare.
            return f"((unsigned)({a}) {cop} ((unsigned)({b}) ^ {self.cmp_nonce}))"
        if kind == "cond":
            # Use the low bit as the controlling bool.  An `& 1u` result is a
            # clean 0/1 value: it triggers neither -Wint-in-bool-context (which
            # fires on `<<`/`*` in bool context) nor -Wtautological-compare
            # (which fires on `!= 0` of provably-nonzero expressions).
            c = self.expr(depth - 1)
            return f"(((unsigned)({c}) & 1u) ? (unsigned)({a}) : (unsigned)({b}))"
        raise AssertionError(kind)

    def _leaf(self) -> str:
        choices = ["const", "const"]
        if self.uvars:
            choices += ["uvar", "uvar"]
        if self.svars:
            choices.append("svar")
        if self.arrays:
            choices.append("array")
        if self.structs:
            choices.append("struct")
        # "ptr" profile: a deref load *p is a valid unsigned operand (feeds load-CSE).
        # Returns DATA, never the address (I9).
        if self.has("ptr") and self.pvars:
            choices.append("deref")
        # "fnptr" profile: an indirect call through the dispatch table is a valid
        # unsigned-valued leaf.  Only offered when the table exists and we are not
        # already inside an icall's args (the _icall_depth guard keeps args shallow
        # and bounds the number of indirect calls).
        if self.has("fnptr") and self.dtab_name and self._icall_depth == 0:
            choices.append("icall")
        kind = self.rng.choice(choices)
        if kind == "const":
            return self.rconst()
        if kind == "uvar":
            return self.rng.choice(self.uvars)
        if kind == "svar":
            name, _ = self.rng.choice(self.svars)
            # Reading a signed var into an unsigned context is value-preserving
            # modulo 2^32 and fully defined.
            return f"(unsigned)({name})"
        if kind == "array":
            name = self.rng.choice(self.arrays)
            idx = self._index_expr()
            return f"{name}[{idx}]"
        if kind == "struct":
            name = self.rng.choice(self.structs)
            f = self.rng.randint(0, STRUCT_FIELDS - 1)
            return f"{name}.f{f}"
        if kind == "deref":
            # Load through a pointer.  Yields the pointee DATA (an unsigned), never
            # the address.  Two *p reads with an intervening *q store feed load-CSE.
            name, _ = self.rng.choice(self.pvars)
            return f"(*{name})"
        if kind == "icall":
            # dtab[idx & (N-1)](a, b) -> unsigned.  All table slots are the
            # generator's own unsigned(unsigned,unsigned) helpers, so the call is
            # exact-prototype (no ABI UB).  Args are shallow exprs (depth 1) with
            # the _icall_depth guard set so they cannot draw further icalls.
            self._icall_depth += 1
            idx = self._index_expr_n(self.dtab_n)
            a = self.expr(1)
            b = self.expr(1)
            self._icall_depth -= 1
            return f"{self.dtab_name}[{idx}]((unsigned)({a}), (unsigned)({b}))"
        raise AssertionError(kind)

    def _index_expr(self) -> str:
        """An array index masked into [0, ARRAY_SIZE)."""
        return self._index_expr_n(ARRAY_SIZE)

    def _index_expr_n(self, n: int) -> str:
        """An index masked into [0, n) where ``n`` is a power of two.

        Used for both array indices (n == ARRAY_SIZE) and the fn-pointer
        dispatch-table index (n == dtab_n).  With n == 1 the mask is ``& 0u`` so
        the index is always 0 (still in range).
        """
        if self.uvars and self.rng.random() < 0.6:
            base = self.rng.choice(self.uvars)
        else:
            base = self.rconst()
        return f"((unsigned)({base}) & {n - 1}u)"

    def has(self, feature: str) -> bool:
        return feature in self.features

    # ----- bitfield generation ("bitfield" profile only) ----------------------

    def _bf_fields(self) -> "list[tuple[str, int]]":
        """A list of (field_name, width) for one bitfield struct.

        All widths come from BF_WIDTHS (every value < 32, so UNSIGNED only and no
        :32 mask edge case) and sum to <= 32 so the non-packed struct stays in a
        single storage unit.  At least one field is always returned.
        """
        n = self.rng.randint(BF_MIN_FIELDS, BF_MAX_FIELDS)
        fields, total = [], 0
        for i in range(n):
            w = self.rng.choice(BF_WIDTHS)
            if total + w > 32:           # keep the non-packed struct in one unit
                break
            fields.append((f"b{i}", w))
            total += w
        if not fields:                   # guarantee at least one field
            fields = [("b0", 1)]
        return fields

    def _bf_mask(self, width: int) -> str:
        """Bit mask keeping an unsigned value in [0, 2**width).  Literal for >=32
        to avoid the `1u << 32` UB (widths are < 32 in practice)."""
        return "0xffffffffu" if width >= 32 else f"((1u << {width}) - 1u)"

    # ----- floating-point generation ("float" profile only) -------------------

    def _fconst(self, ctype: str) -> str:
        """An EXACT, finite, normal FP literal of type ``ctype``.

        Value = sign * m * 2**e with m < 2**24, so it is representable without
        rounding in *both* float and double -> the literal parses to identical
        bits on tcc and gcc (no parser-rounding false positives).  Emitted as a
        C99 hex-float literal (Python ``float.hex()`` is exactly that syntax).
        """
        m = self.rng.randint(0, (1 << FP_MANT_BITS) - 1)
        e = self.rng.randint(FP_EXP_LO, FP_EXP_HI)
        sign = self.rng.choice((1.0, -1.0))
        val = sign * m * (2.0 ** e)        # exact in IEEE double (and float)
        lit = val.hex()                    # e.g. '-0x1.8000000000000p+3'
        return f"{lit}f" if ctype == "float" else lit

    def _fleaf(self, ctype: str) -> str:
        """An FP operand of type ``ctype`` with no side effects.

        One of: an exact constant, an existing FP var (float<->double casts are
        defined), or an int->float cast of an unsigned value (always defined,
        correctly rounded identically on both compilers).
        """
        choices = ["fconst", "fconst"]
        if self.fvars:
            choices += ["fvar", "fvar"]
        if self.uvars:
            choices.append("ucast")
        kind = self.rng.choice(choices)
        if kind == "fconst":
            return self._fconst(ctype)
        if kind == "fvar":
            name, _ = self.rng.choice(self.fvars)
            return f"({ctype})({name})"
        return f"({ctype})((unsigned)({self.rng.choice(self.uvars)}))"

    def _fclamp(self, pad: str, name: str, ctype: str) -> str:
        """Bound |name| <= 2**40 so loop-carried FP can never reach Inf.

        The bound is a power of two (exact); the compare/select also exercises FP
        comparison + select codegen.  Result stays finite -> bits stay portable.
        """
        sfx = "f" if ctype == "float" else ""
        big = f"0x1p40{sfx}"
        return (f"{pad}{name} = ({name} < -{big} || {name} > {big}) "
                f"? ({ctype})1 : {name};")

    # ----- statement generation ------------------------------------------------

    def block(self, depth: int, indent: int) -> list[str]:
        lines: list[str] = []
        n = self.rng.randint(1, MAX_STMTS_PER_BLOCK)
        for _ in range(n):
            lines += self.statement(depth, indent)
        return lines

    def _case_body(self, depth: int, indent: int) -> list[str]:
        """One switch-arm body ("switch" profile): 0-2 ordinary statements (reusing
        the existing assign/checksum vocabulary, so the arm exercises register
        pressure across the dispatch) followed by a distinct cs-fold so the arm is
        always output-defined and distinguishable.  Declares no new variables, so
        no fall-through could skip an initialization a later read needs."""
        pad = "  " * indent
        lines: list[str] = []
        for _ in range(self.rng.randint(0, 2)):
            lines += self.statement(max(depth - 1, 0), indent)
        lines.append(f"{pad}cs = csmix(cs, {self.rconst()});")
        return lines

    def statement(self, depth: int, indent: int) -> list[str]:
        pad = "  " * indent
        opts = ["assign", "assign", "checksum", "checksum"]
        if depth > 0:
            opts += ["if", "for", "while"]
        if self.arrays:
            opts.append("arraystore")
        if self.structs:
            opts.append("structstore")
        if self.has("float") and self.fvars:
            opts += ["fassign", "fassign", "fcmp"]
        # "fnptr" profile: fold an indirect-call result straight into the checksum
        # (guarantees output-sensitivity even when the icall leaf is not sampled).
        if self.has("fnptr") and self.dtab_name:
            opts += ["icall_cs", "icall_cs"]
        # "bitfield" profile: write a bitfield member (RHS masked to its width).
        if self.has("bitfield") and self.bfvars:
            opts += ["bfstore", "bfstore"]
        # "switch" profile: dense/sparse switch + forward goto (depth-gated like if/for/while).
        if self.has("switch") and depth > 0:
            opts += ["switch_dense", "switch_sparse", "goto_fwd"]
        # "struct_byval" profile: by-value struct helper call, and a same-member union.
        if self.has("struct_byval") and self.callable_sbhelpers:
            opts += ["sbcall", "sbcall"]
        if self.has("struct_byval"):
            opts.append("uniongate")
        # "varargs" profile: a variadic vsum(n, ...) call with a varying arg count.
        if self.has("varargs"):
            opts += ["vcall", "vcall"]
        # "ptr" profile: deref store (+read-back) and an alias store-one/load-other.
        if self.has("ptr") and self.pvars:
            opts += ["ptrstore", "aliasrw"]
        kind = self.rng.choice(opts)

        if kind == "assign":
            if not self.uvars:
                return self.statement(depth, indent)  # nothing to assign to
            v = self.rng.choice(self.uvars)
            return [f"{pad}{v} = (unsigned)({self.expr(MAX_EXPR_DEPTH)}) & 0xffffffffu;"]

        if kind == "checksum":
            return [f"{pad}cs = csmix(cs, (unsigned)({self.expr(MAX_EXPR_DEPTH)}));"]

        if kind == "icall_cs":
            # Indirect call whose result is mixed into cs.  cs is also passed as the
            # 2nd arg so the call is sensitive to prior state.  cs is in scope here
            # (statement() only runs inside main(), never in a helper body).
            idx = self._index_expr_n(self.dtab_n)
            arg = self.expr(MAX_EXPR_DEPTH)
            return [f"{pad}cs = csmix(cs, {self.dtab_name}[{idx}]"
                    f"((unsigned)({arg}), cs));"]

        if kind == "arraystore":
            name = self.rng.choice(self.arrays)
            idx = self._index_expr()
            return [f"{pad}{name}[{idx}] = (unsigned)({self.expr(MAX_EXPR_DEPTH)});"]

        if kind == "structstore":
            name = self.rng.choice(self.structs)
            f = self.rng.randint(0, STRUCT_FIELDS - 1)
            return [f"{pad}{name}.f{f} = (unsigned)({self.expr(MAX_EXPR_DEPTH)});"]

        if kind == "bfstore":
            # Write one bitfield member; the RHS is masked to the field's width so
            # intended == read-back and a width-truncation codegen bug is visible
            # (C would otherwise silently truncate and hide it).  Unsigned only.
            name, _ty, fields = self.rng.choice(self.bfvars)
            fname, w = self.rng.choice(fields)
            rhs = self.expr(MAX_EXPR_DEPTH)
            return [f"{pad}{name}.{fname} = (unsigned)({rhs}) & {self._bf_mask(w)};"]

        if kind == "sbcall":
            # By-value struct pass + (possibly different-shape) struct return; fold
            # each NAMED field of the result into cs.  The param struct is built
            # fully-initialised inline (no uninit read, no escaping address).
            hn, pj, rk = self.rng.choice(self.callable_sbhelpers)
            a = self.fresh("sba")
            t = self.fresh("sbt")
            ainits = ", ".join(f"(unsigned)({self.expr(2)}) & {_sb_field_mask(ct)}"
                               for _fn, ct in SB_FIELDS[pj])
            lines = [f"{pad}{{ struct {pj} {a} = {{ {ainits} }};",
                     f"{pad}  struct {rk} {t} = {hn}({a}, (unsigned)({self.expr(MAX_EXPR_DEPTH)}));"]
            for fn, _ct in SB_FIELDS[rk]:
                lines.append(f"{pad}  cs = csmix(cs, {t}.{fn});")
            lines.append(f"{pad}}}")
            return lines

        if kind == "uniongate":
            # Write member w, read member w (SAME member -> no type-punning UB).
            u = self.fresh("ub")
            return [f"{pad}{{ union UB {u}; {u}.w = (unsigned)({self.expr(3)});"
                    f" cs = csmix(cs, {u}.w); }}"]

        if kind == "vcall":
            # One draw -> count == n == number of trailing args (cannot desync).
            # All variadic args are int (its own promotion -> no ABI ambiguity);
            # vsum reads exactly n of them.  k>=4 spills past r0-r3 onto the stack.
            k = self.rng.randint(0, MAX_VARARGS)
            args = ", ".join(f"(int)({self.expr(MAX_EXPR_DEPTH)})" for _ in range(k))
            sep = ", " if k else ""
            return [f"{pad}cs = csmix(cs, vsum({k}u{sep}{args}));"]

        if kind == "ptrstore":
            # Deref STORE of a defined unsigned, then checksum the read-back.  Only
            # the pointee DATA reaches cs (I9); the pointer value never does.
            name, _ = self.rng.choice(self.pvars)
            return [f"{pad}*{name} = (unsigned)({self.expr(MAX_EXPR_DEPTH)});",
                    f"{pad}cs = csmix(cs, *{name});"]

        if kind == "aliasrw":
            # Store via one pointer, load via another that MAY alias it, then swap:
            # the canonical store-forwarding / DSE / load-CSE trigger.  Both reads
            # must observe the most recent aliasing store; a wrong no-alias
            # assumption picks up a stale value and cs diverges from tcc -O0.
            if len(self.pvars) >= 2:
                p, q = self.rng.sample(self.pvars, 2)
            else:
                p = q = self.pvars[0]
            return [f"{pad}*{p[0]} = (unsigned)({self.expr(MAX_EXPR_DEPTH)});",
                    f"{pad}cs = csmix(cs, *{q[0]});",
                    f"{pad}*{q[0]} = (unsigned)({self.expr(MAX_EXPR_DEPTH)});",
                    f"{pad}cs = csmix(cs, *{p[0]});"]

        if kind == "fassign":
            # Three-address single op (no a*b+c pattern to fuse) -> isolates each
            # softfloat routine and keeps every intermediate rounded to nominal
            # precision.  Result is clamped so it can never grow to Inf.
            name, ctype = self.rng.choice(self.fvars)
            op = self.rng.choice(["+", "-", "*", "/", "neg"])
            if op == "neg":
                rhs = f"-({self._fleaf(ctype)})"
            elif op == "/":
                a, b = self._fleaf(ctype), self._fleaf(ctype)
                # Force a nonzero divisor: never 0.0/0.0 (NaN) or x/0.0 (Inf).
                rhs = f"({a}) / ((({b}) == ({ctype})0) ? ({ctype})1 : ({b}))"
            else:
                a, b = self._fleaf(ctype), self._fleaf(ctype)
                rhs = f"({a}) {op} ({b})"
            return [f"{pad}{name} = {rhs};", self._fclamp(pad, name, ctype)]

        if kind == "fcmp":
            # Fold a finite, non-NaN FP comparison (portable 0/1) into the
            # checksum -> exercises FP compare + the int<-bool path.
            ctype = self.rng.choice(FP_TYPES)
            cop = self.rng.choice(["<", ">", "<=", ">=", "==", "!="])
            a, b = self._fleaf(ctype), self._fleaf(ctype)
            return [f"{pad}cs = csmix(cs, (({a}) {cop} ({b})) ? 1u : 0u);"]

        if kind == "if":
            cond = self.expr(MAX_EXPR_DEPTH)
            lines = [f"{pad}if ((unsigned)({cond}) & 1u) {{"]
            lines += self.block(depth - 1, indent + 1)
            if self.rng.random() < 0.5:
                lines.append(f"{pad}}} else {{")
                lines += self.block(depth - 1, indent + 1)
            lines.append(f"{pad}}}")
            return lines

        if kind in ("for", "while"):
            # Bounded loop: fresh counter, compile-time bound, body cannot extend
            # it (the counter is incremented only by the loop machinery and the
            # body is not allowed to assign to it -- it is added to the readable
            # var set, never to an assignment target since assignments pick from
            # self.uvars which includes it, BUT the bound check uses the literal
            # trip and the increment is fixed, so even if the body writes it the
            # `< trip` test plus `++` still terminates in <= trip iterations...
            # to be fully safe we simply do NOT expose the counter as an
            # assignment target: it is read-only because every assignment masks
            # and the loop test is on the counter which the body can read but the
            # generator's assign-statement could overwrite.  Guarantee termination
            # by using a SEPARATE hidden guard counter the body can never name.)
            it = self.fresh("i")        # readable index (body may read, may write)
            guard = self.fresh("g")     # hidden guard, body can never reference it
            trip = self.rng.randint(1, MAX_LOOP_TRIP)
            saved = list(self.uvars)
            self.uvars.append(it)       # body may read/modify the index freely
            if kind == "for":
                lines = [f"{pad}for (unsigned {guard} = 0u; {guard} < {trip}u; "
                         f"{guard}++) {{"]
                lines.append(f"{pad}  unsigned {it} = {guard};")
                # Fold the index into the checksum so it is always 'used' (no
                # unused-variable warning) and the loop is output-sensitive.
                lines.append(f"{pad}  cs = csmix(cs, {it});")
                lines += self.block(depth - 1, indent + 1)
                lines.append(f"{pad}}}")
            else:
                lines = [f"{pad}{{ unsigned {guard} = 0u;"]
                lines.append(f"{pad}  while ({guard} < {trip}u) {{")
                lines.append(f"{pad}    unsigned {it} = {guard};")
                lines.append(f"{pad}    cs = csmix(cs, {it});")
                lines += self.block(depth - 1, indent + 2)
                lines.append(f"{pad}    {guard}++;")
                lines.append(f"{pad}  }}")
                lines.append(f"{pad}}}")
            self.uvars = saved
            return lines

        if kind == "switch_dense":
            # Consecutive cases 0..K-1 (>=4 -> 100% density -> jump-table path at
            # O1+).  Selector masked into [0, mask+1); when K is a power of two the
            # mask domain == the label set (default dead); otherwise masked values
            # K..2^ceil-1 fall to the (output-defined) default.  Every value hits
            # a real arm or default -> no undefined dispatch.
            K = self.rng.randint(SWITCH_DENSE_MIN, SWITCH_DENSE_MAX)
            mask = K - 1 if (K & (K - 1)) == 0 else (1 << K.bit_length()) - 1
            sel = self.fresh("sel")
            lines = [f"{pad}{{ unsigned {sel} = (unsigned)({self.expr(MAX_EXPR_DEPTH)}) & {mask}u;",
                     f"{pad}  switch ({sel}) {{"]
            for c in range(K):
                lines.append(f"{pad}  case {c}:")
                lines += self._case_body(depth, indent + 2)
                lines.append(f"{pad}    break;")
            lines += [f"{pad}  default: cs = csmix(cs, {self.small_const()}u); break;",
                      f"{pad}  }} }}"]
            return lines

        if kind == "switch_sparse":
            # Scattered labels in a power-of-two window -> density < 50% forces the
            # gcase() if-chain/binary-search path.  rng.sample gives a SET (no
            # duplicate case values).  Most masked values miss every label and hit
            # the load-bearing default (which folds into cs -> output-defined).
            n = self.rng.randint(SWITCH_SPARSE_MIN, SWITCH_SPARSE_MAX)
            labels = self.rng.sample(range(SWITCH_SPARSE_WINDOW), n)
            sel = self.fresh("sel")
            lines = [f"{pad}{{ unsigned {sel} = (unsigned)({self.expr(MAX_EXPR_DEPTH)}) "
                     f"& {SWITCH_SPARSE_WINDOW - 1}u;",
                     f"{pad}  switch ({sel}) {{"]
            for v in sorted(labels):
                lines.append(f"{pad}  case {v}:")
                lines += self._case_body(depth, indent + 2)
                lines.append(f"{pad}    break;")
            lines += [f"{pad}  default: cs = csmix(cs, {self.small_const()}u); break;",
                      f"{pad}  }} }}"]
            return lines

        if kind == "goto_fwd":
            # Forward-only goto over a declaration-free, cs-folding region: skipping
            # it cannot bypass any initialization a later read needs, and no backward
            # edge is ever created (no generated loop -> bounded & terminating).
            lbl = self.fresh_label()
            g = self.fresh("g")          # hidden guard; never an assignment target
            lines = [f"{pad}{{ unsigned {g} = (unsigned)({self.expr(MAX_EXPR_DEPTH)}) & 1u;",
                     f"{pad}  if ({g}) goto {lbl};"]
            for _ in range(self.rng.randint(1, 3)):
                lines.append(f"{pad}  cs = csmix(cs, (unsigned)({self.expr(MAX_EXPR_DEPTH)}));")
            lines += [f"{pad}{lbl}:;",
                      f"{pad}  cs = csmix(cs, {self.small_const()}u); }}"]
            return lines

        raise AssertionError(kind)


# ---------------------------------------------------------------------------
# Top-level program assembly
# ---------------------------------------------------------------------------

def _prologue(seed: int, features=frozenset()) -> str:
    # NB: when ``features`` is empty this is BYTE-IDENTICAL to the historical
    # prologue (extra_inc == "" and no helpers appended).  Do not reorder.
    extra_inc = ("#include <string.h>\n" if "float" in features else "") \
              + ("#include <stdarg.h>\n" if "varargs" in features else "")
    base = (
        f"/* AUTO-GENERATED by tests/fuzz/gen_c.py  seed={seed}\n"
        " * UB-free random C program for differential fuzzing (Tracks 2/3).\n"
        ' * Prints a single line: "checksum=<hex>".  Do not edit by hand.\n'
        " */\n"
        "#include <stdio.h>\n"
        f"{extra_inc}"
        "\n"
        "/* Rolling checksum mix (all unsigned -> fully defined). */\n"
        "static unsigned csmix(unsigned h, unsigned v)\n"
        "{\n"
        "  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);\n"
        "  h = (h << 13) | (h >> 19);\n"
        "  return h * 2654435761u;\n"
        "}\n"
    )
    if "float" in features:
        # Reinterpret FP bits -> unsigned via memcpy: no aliasing UB, and the
        # store to a nominal-width object rounds away any excess precision, so
        # the folded value is identical across compilers/optimisation levels.
        base += (
            "\n"
            "static unsigned fbits_d(double d){ unsigned u[2]; "
            "memcpy(u, &d, sizeof u); return csmix(u[0], u[1]); }\n"
            "static unsigned fbits_f(float f){ unsigned u; "
            "memcpy(&u, &f, sizeof u); return u; }\n"
        )
    if "varargs" in features:
        # Sum n int args; read each with va_arg(ap,int) — int is its own default
        # promotion, so caller-passed type == callee-read type (no ABI ambiguity).
        # acc is unsigned -> defined modular wraparound; va_end always paired.
        base += (
            "\n"
            "/* varargs profile: sum n int args (acc unsigned -> defined wraparound). */\n"
            "static unsigned vsum(unsigned n, ...)\n"
            "{\n"
            "  va_list ap; unsigned acc = 0u; unsigned i;\n"
            "  va_start(ap, n);\n"
            "  for (i = 0u; i < n; i++) acc += (unsigned)va_arg(ap, int);\n"
            "  va_end(ap);\n"
            "  return acc;\n"
            "}\n"
        )
    return base


def _emit_helper(g: Gen, name: str) -> str:
    """An unsigned(unsigned,unsigned) helper with a few safe statements.

    Helpers may only call *earlier* helpers (strict DAG call graph) so there is
    no recursion and every helper terminates -> no stack-overflow / runaway UB.
    """
    # Local scope: shadow the generator's live-var set with the two params.
    saved_u = g.uvars
    saved_a = g.arrays
    saved_s = g.structs
    saved_v = g.svars
    saved_callable = g.callable_helpers
    saved_nonce = g.cmp_nonce
    g.uvars = ["pa", "pb", "lr"]
    g.arrays = []
    g.structs = []
    g.svars = []
    g.cmp_nonce = "lr"   # the in-scope runtime accumulator inside a helper body
    # Only previously-defined helpers (everything in g.helpers except this one,
    # which has not been appended yet at call time -> g.helpers holds the prior).
    g.callable_helpers = list(g.helpers)
    body = []
    # Seed lr from both params so neither parameter is unused (-Wunused-parameter)
    # and the body is sensitive to its inputs.
    body.append("  unsigned lr = pa ^ (pb * 3u);")
    n = g.rng.randint(2, 5)
    for _ in range(n):
        which = g.rng.choice(["acc", "acc", "branch"])
        if which == "acc":
            body.append(f"  lr = (unsigned)({g.expr(3)});")
        else:
            body.append(f"  if ((unsigned)({g.expr(2)}) & 1u) "
                        f"lr += (unsigned)({g.expr(2)});")
    # Always fold lr into the return value so it is never "set but not used"
    # (-Wunused-but-set-variable) and the result depends on the accumulated body.
    body.append(f"  return (unsigned)({g.expr(3)}) ^ lr;")
    g.uvars = saved_u
    g.arrays = saved_a
    g.structs = saved_s
    g.svars = saved_v
    g.callable_helpers = saved_callable
    g.cmp_nonce = saved_nonce
    lines = [f"static unsigned {name}(unsigned pa, unsigned pb)", "{"]
    lines += body
    lines += ["}"]
    return "\n".join(lines)


def _emit_struct_helper(g: Gen, name: str):
    """A by-value struct helper: ``struct {rk} name(struct {pj} p, unsigned x)``.

    Takes a struct by value and returns a (possibly different-shape) struct by
    value, so the same program exercises both the reg-return and sret paths on the
    producing side.  Body is pure ALU over the param's named fields + x (all
    initialised by the caller) -> no recursion, no scalar-helper calls, terminating.
    Returns (text, param_shape, ret_shape).
    """
    pj_name, pj_fields = g.rng.choice(SB_SHAPES)
    rk_name, rk_fields = g.rng.choice(SB_SHAPES)
    saved = (g.uvars, g.arrays, g.structs, g.svars, g.callable_helpers, g.cmp_nonce)
    # Readable values inside the body: the param's named fields + x.  cmp_nonce=x
    # keeps comparison operands non-identical; no scalar-helper calls here.
    g.uvars = [f"p.{fn}" for fn, _ in pj_fields] + ["x"]
    g.arrays, g.structs, g.svars, g.callable_helpers, g.cmp_nonce = [], [], [], [], "x"
    first_pf = pj_fields[0][0]
    inits = []
    for idx, (fn, ct) in enumerate(rk_fields):
        if idx == 0:
            # Seed field 0 from BOTH params so neither p nor x is unused.
            inits.append(f"(unsigned)(x ^ (p.{first_pf} * 3u)) & {_sb_field_mask(ct)}")
        else:
            inits.append(f"(unsigned)({g.expr(3)}) & {_sb_field_mask(ct)}")
    body = [f"  struct {rk_name} r = {{ {', '.join(inits)} }};"]
    for _ in range(g.rng.randint(1, 3)):
        fn, ct = g.rng.choice(rk_fields)
        body.append(f"  r.{fn} = (unsigned)({g.expr(3)}) & {_sb_field_mask(ct)};")
    body.append("  return r;")
    g.uvars, g.arrays, g.structs, g.svars, g.callable_helpers, g.cmp_nonce = saved
    lines = [f"static struct {rk_name} {name}(struct {pj_name} p, unsigned x)", "{"]
    lines += body + ["}"]
    return "\n".join(lines), pj_name, rk_name


def generate_program(seed: int, profile: str = DEFAULT_PROFILE) -> str:
    """Return the full C source for a UB-free random program for ``seed``.

    ``profile`` selects a feature set (see ``PROFILES``).  The default ``"int"``
    profile is byte-identical to the historical generator.
    """
    features = PROFILES[profile] if isinstance(profile, str) else frozenset(profile)
    g = Gen(seed, features)
    out: list[str] = [_prologue(seed, features)]

    # --- helper functions (declared before main so calls are in scope) ---
    # Append each name to g.helpers only AFTER its body is emitted, so a helper
    # can only call strictly-earlier helpers (no self/forward recursion).
    n_helpers = g.rng.randint(0, MAX_HELPERS)
    helper_defs = []
    for _ in range(n_helpers):
        name = g.fresh("helper")
        helper_defs.append(_emit_helper(g, name))
        g.helpers.append(name)
    out += helper_defs

    # --- struct_byval: shape type decls + by-value struct helpers ---
    # Type decls must precede the helpers (used in their signatures).  At least one
    # struct helper is always emitted so every shape used by a helper is referenced
    # and no static helper is unused.
    if g.has("struct_byval"):
        for sname, sfields in SB_SHAPES:
            decls = " ".join(f"{ct} {fn};" for fn, ct in sfields)
            out.append(f"struct {sname} {{ {decls} }};")
        out.append("union UB { unsigned w; unsigned char b; };")
        n_sb = g.rng.randint(1, MAX_STRUCT_HELPERS)
        for _ in range(n_sb):
            nm = g.fresh("sbh")
            text, pj, rk = _emit_struct_helper(g, nm)
            out.append(text)
            g.sbhelpers.append((nm, pj, rk))

    # Inside main(), every helper is callable (the DAG restriction only applied
    # while emitting helper bodies).
    g.callable_helpers = list(g.helpers)
    g.callable_sbhelpers = list(g.sbhelpers)

    # --- struct type (single shape reused) ---
    struct_fields = "\n".join(f"  unsigned f{i};" for i in range(STRUCT_FIELDS))
    out.append(f"struct S {{\n{struct_fields}\n}};")

    # --- bitfield struct types ("bitfield" profile) ---
    # A non-packed type (natural alignment -> aligned insert/extract path) and a
    # packed variant (#pragma pack(1) + __attribute__((packed)) -> some fields
    # straddle bytes -> load/store_packed_bf).  UNSIGNED fields only; the field
    # layouts are remembered in g._bf_types for instance decls + the final fold.
    if g.has("bitfield"):
        f_np = g._bf_fields()
        np_decl = "\n".join(f"  unsigned {nm} : {w};" for nm, w in f_np)
        out.append(f"struct BF {{\n{np_decl}\n}};")
        f_pk = g._bf_fields()
        pk_decl = "\n".join(f"  unsigned {nm} : {w};" for nm, w in f_pk)
        out.append(
            "#pragma pack(push, 1)\n"
            f"struct BFP {{\n{pk_decl}\n}} __attribute__((packed));\n"
            "#pragma pack(pop)"
        )
        g._bf_types = [("struct BF", f_np), ("struct BFP", f_pk)]

    # --- main ---
    main: list[str] = ["int main(void)", "{", "  unsigned cs = 0x12345678u;"]

    # Signed globals (read-only sources): initialised from constants, value
    # masked to the storage width so the stored value is identical on tcc/gcc.
    n_signed = g.rng.randint(1, 3)
    for _ in range(n_signed):
        ctype = g.rng.choice(SIGNED_TYPES)
        name = g.fresh("s")
        mask = _mask_for_type(ctype)
        val = g.rng.randint(0, 0x7FFFFFFF)
        main.append(f"  {ctype} {name} = ({ctype})({val}u & {mask});")
        g.svars.append((name, ctype))

    # Unsigned live scalars.
    n_u = g.rng.randint(2, MAX_GLOBAL_VARS)
    for _ in range(n_u):
        name = g.fresh("u")
        main.append(f"  unsigned {name} = {g.rconst()};")
        g.uvars.append(name)

    # Arrays (fully initialised, power-of-two size).
    n_arr = g.rng.randint(0, 2)
    for _ in range(n_arr):
        name = g.fresh("arr")
        inits = ", ".join(g.rconst() for _ in range(ARRAY_SIZE))
        main.append(f"  unsigned {name}[{ARRAY_SIZE}] = {{ {inits} }};")
        g.arrays.append(name)

    # Pointers ("ptr" profile): declared AFTER all unsigned scalars/arrays are
    # initialised (I7), as function-lifetime locals (I3), each a single-level
    # `unsigned *` at an `unsigned` pointee (I1/I5).  Array targets use a fixed
    # in-bounds index (I6: offset-0, the high element, or a captured runtime base).
    # A deliberate alias pair points two pointers at the SAME object (I8).  The
    # pointer is only ever used as *p (I9) and never escapes (I2).
    if g.has("ptr"):
        targets = list(g.uvars)
        for a in g.arrays:
            targets.append(f"{a}[0u]")                      # offset-0 (DEREF-marker seam)
            targets.append(f"{a}[{ARRAY_SIZE - 1}u]")       # fixed high element
            if g.uvars:                                     # captured runtime base
                base = g.rng.choice(g.uvars)
                targets.append(f"{a}[((unsigned)({base}) & {ARRAY_SIZE - 1}u)]")
        if targets:
            for _ in range(g.rng.randint(1, 3)):
                tgt = g.rng.choice(targets)
                name = g.fresh("p")
                main.append(f"  unsigned *{name} = &{tgt};")
                g.pvars.append((name, tgt))
            # Deliberate alias pair: a SECOND pointer to an already-targeted object.
            if g.rng.random() < 0.6 and g.pvars:
                _, dup = g.rng.choice(g.pvars)
                name = g.fresh("p")
                main.append(f"  unsigned *{name} = &{dup};")
                g.pvars.append((name, dup))

    # Structs (all fields initialised).
    n_st = g.rng.randint(0, 2)
    for _ in range(n_st):
        name = g.fresh("st")
        inits = ", ".join(g.rconst() for _ in range(STRUCT_FIELDS))
        main.append(f"  struct S {name} = {{ {inits} }};")
        g.structs.append(name)

    # Bitfield struct instances ("bitfield" profile): every field brace-init to 0u
    # (no uninitialised field/padding is ever read).
    if g.has("bitfield"):
        n_bf = g.rng.randint(1, 2)
        for _ in range(n_bf):
            tyname, fields = g.rng.choice(g._bf_types)
            name = g.fresh("bf")
            inits = ", ".join("0u" for _ in fields)
            main.append(f"  {tyname} {name} = {{ {inits} }};")
            g.bfvars.append((name, tyname, fields))

    # FP locals ("float" profile).  Force at least one of each width so both
    # fbits_* reinterpret helpers are always referenced (-Wunused-function).
    if g.has("float"):
        n_fp = g.rng.randint(2, MAX_FP_VARS)
        for i in range(n_fp):
            ctype = ("double", "float")[i] if i < 2 else g.rng.choice(FP_TYPES)
            name = g.fresh("f")
            main.append(f"  {ctype} {name} = {g._fconst(ctype)};")
            g.fvars.append((name, ctype))

    # FP-pointer dispatch table ("fnptr" profile).  Declared once, after the
    # helpers exist, as a static-const local array of N (power of two) slots filled
    # round-robin from g.helpers so every slot is a real unsigned(unsigned,unsigned)
    # helper.  static const keeps the pointers immutable (stresses devirt/CSE) and
    # needs no runtime init.  Requires >=1 helper; otherwise nothing is emitted.
    if g.has("fnptr") and g.helpers:
        n = 1
        while n < min(len(g.helpers), MAX_DTAB):
            n <<= 1
        slots = [g.helpers[i % len(g.helpers)] for i in range(n)]
        name = g.fresh("dtab")
        main.append(f"  static unsigned (*const {name}[{n}])(unsigned, unsigned)"
                    f" = {{ {', '.join(slots)} }};")
        g.dtab_name, g.dtab_n = name, n

    main.append("")
    # Body: a handful of statements / control flow.
    main += g.block(depth=2, indent=1)
    main.append("")

    # Fold every live variable / aggregate into the checksum so the result is
    # sensitive to the final state of everything we computed.
    for v in g.uvars:
        main.append(f"  cs = csmix(cs, {v});")
    # Call every helper at least once with deterministic args so no helper is
    # unused (-Wunused-function) and the result depends on helper codegen too.
    for i, name in enumerate(g.helpers):
        main.append(f"  cs = csmix(cs, {name}({(i * 0x1234567 + 1) & 0xFFFFFFFF}u, cs));")
    # Guarantee the dispatch table is referenced at least once (never set-but-unused
    # if a seed's body happened to sample no icall), with a deterministic call.
    if g.dtab_name:
        main.append(f"  cs = csmix(cs, {g.dtab_name}[0](1u, cs));")
    # Guarantee vsum is referenced (no -Wunused-function) and the program is
    # output-sensitive to variadic codegen even if no vcall was sampled.
    if g.has("varargs"):
        main.append("  cs = csmix(cs, vsum(2u, 1, (int)cs));")
    for name, _ in g.svars:
        main.append(f"  cs = csmix(cs, (unsigned){name});")
    for name in g.arrays:
        main.append(f"  for (unsigned k = 0u; k < {ARRAY_SIZE}u; k++) "
                    f"cs = csmix(cs, {name}[k]);")
    for name in g.structs:
        for i in range(STRUCT_FIELDS):
            main.append(f"  cs = csmix(cs, {name}.f{i});")
    # Fold each NAMED bitfield member into cs (NEVER raw bytes -- inter-field and
    # packed-pad bits are indeterminate and would be a false positive).
    for name, _ty, fields in g.bfvars:
        for fname, _w in fields:
            main.append(f"  cs = csmix(cs, {name}.{fname});")
    # Fold each pointer's final pointee value via *p (DATA, never the address --
    # I9), so every pointer is used (no -Wunused-variable) and the program is
    # sensitive to the last store through it.
    for name, _ in g.pvars:
        main.append(f"  cs = csmix(cs, *{name});")
    # Fold each FP var's exact bit pattern into the checksum.
    for name, ctype in g.fvars:
        helper = "fbits_f" if ctype == "float" else "fbits_d"
        main.append(f"  cs = csmix(cs, {helper}({name}));")
    # Call every struct helper once deterministically (so none is set-but-unused)
    # and fold each returned struct's NAMED fields into cs.
    for i, (hn, pj, rk) in enumerate(g.sbhelpers):
        a = g.fresh("sba")
        t = g.fresh("sbt")
        ainits = ", ".join(f"{(i * 0x1234567 + j + 1) & 0xff}u" if "char" in ct
                           else f"{(i * 0x1234567 + j + 1) & 0xFFFFFFFF}u"
                           for j, (_fn, ct) in enumerate(SB_FIELDS[pj]))
        main.append(f"  {{ struct {pj} {a} = {{ {ainits} }};")
        main.append(f"    struct {rk} {t} = {hn}({a}, cs);")
        for fn, _ct in SB_FIELDS[rk]:
            main.append(f"    cs = csmix(cs, {t}.{fn}); }}" if (fn, _ct) == SB_FIELDS[rk][-1]
                        else f"    cs = csmix(cs, {t}.{fn});")

    main.append('  printf("checksum=%08x\\n", cs);')
    main.append("  return 0;")
    main.append("}")

    out.append("\n".join(main))
    return "\n\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=0, help="RNG seed (default 0)")
    ap.add_argument("--profile", choices=sorted(PROFILES), default=DEFAULT_PROFILE,
                    help="feature profile (default 'int' = byte-identical historical stream)")
    ap.add_argument("-o", "--output", type=str, default=None,
                    help="write the program to this file (default: stdout)")
    ap.add_argument("--count", type=int, default=0,
                    help="generate COUNT programs for seeds [seed, seed+COUNT)")
    ap.add_argument("--out-dir", type=str, default=None,
                    help="directory for --count output (files fuzz_<seed>.c)")
    args = ap.parse_args(argv)

    if args.count > 0:
        out_dir = Path(args.out_dir or ".")
        out_dir.mkdir(parents=True, exist_ok=True)
        for s in range(args.seed, args.seed + args.count):
            src = generate_program(s, args.profile)
            (out_dir / f"fuzz_{s}.c").write_text(src)
        print(f"wrote {args.count} programs to {out_dir}", file=sys.stderr)
        return 0

    src = generate_program(args.seed, args.profile)
    if args.output:
        Path(args.output).write_text(src)
    else:
        sys.stdout.write(src)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
