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
    "int":   frozenset(),                 # default — DO NOT change its stream
    "float": frozenset({"float"}),        # adds double/float arithmetic
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
        self.helpers: list[str] = []     # all helper function names (unsigned->unsigned)
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
        self._counter = 0

    def fresh(self, prefix: str) -> str:
        self._counter += 1
        return f"{prefix}{self._counter}"

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
        raise AssertionError(kind)

    def _index_expr(self) -> str:
        """An array index masked into [0, ARRAY_SIZE)."""
        if self.uvars and self.rng.random() < 0.6:
            base = self.rng.choice(self.uvars)
        else:
            base = self.rconst()
        return f"((unsigned)({base}) & {ARRAY_SIZE - 1}u)"

    def has(self, feature: str) -> bool:
        return feature in self.features

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
        kind = self.rng.choice(opts)

        if kind == "assign":
            if not self.uvars:
                return self.statement(depth, indent)  # nothing to assign to
            v = self.rng.choice(self.uvars)
            return [f"{pad}{v} = (unsigned)({self.expr(MAX_EXPR_DEPTH)}) & 0xffffffffu;"]

        if kind == "checksum":
            return [f"{pad}cs = csmix(cs, (unsigned)({self.expr(MAX_EXPR_DEPTH)}));"]

        if kind == "arraystore":
            name = self.rng.choice(self.arrays)
            idx = self._index_expr()
            return [f"{pad}{name}[{idx}] = (unsigned)({self.expr(MAX_EXPR_DEPTH)});"]

        if kind == "structstore":
            name = self.rng.choice(self.structs)
            f = self.rng.randint(0, STRUCT_FIELDS - 1)
            return [f"{pad}{name}.f{f} = (unsigned)({self.expr(MAX_EXPR_DEPTH)});"]

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

        raise AssertionError(kind)


# ---------------------------------------------------------------------------
# Top-level program assembly
# ---------------------------------------------------------------------------

def _prologue(seed: int, features=frozenset()) -> str:
    # NB: when ``features`` is empty this is BYTE-IDENTICAL to the historical
    # prologue (extra_inc == "" and no helpers appended).  Do not reorder.
    extra_inc = "#include <string.h>\n" if "float" in features else ""
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

    # Inside main(), every helper is callable (the DAG restriction only applied
    # while emitting helper bodies).
    g.callable_helpers = list(g.helpers)

    # --- struct type (single shape reused) ---
    struct_fields = "\n".join(f"  unsigned f{i};" for i in range(STRUCT_FIELDS))
    out.append(f"struct S {{\n{struct_fields}\n}};")

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

    # Structs (all fields initialised).
    n_st = g.rng.randint(0, 2)
    for _ in range(n_st):
        name = g.fresh("st")
        inits = ", ".join(g.rconst() for _ in range(STRUCT_FIELDS))
        main.append(f"  struct S {name} = {{ {inits} }};")
        g.structs.append(name)

    # FP locals ("float" profile).  Force at least one of each width so both
    # fbits_* reinterpret helpers are always referenced (-Wunused-function).
    if g.has("float"):
        n_fp = g.rng.randint(2, MAX_FP_VARS)
        for i in range(n_fp):
            ctype = ("double", "float")[i] if i < 2 else g.rng.choice(FP_TYPES)
            name = g.fresh("f")
            main.append(f"  {ctype} {name} = {g._fconst(ctype)};")
            g.fvars.append((name, ctype))

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
    for name, _ in g.svars:
        main.append(f"  cs = csmix(cs, (unsigned){name});")
    for name in g.arrays:
        main.append(f"  for (unsigned k = 0u; k < {ARRAY_SIZE}u; k++) "
                    f"cs = csmix(cs, {name}[k]);")
    for name in g.structs:
        for i in range(STRUCT_FIELDS):
            main.append(f"  cs = csmix(cs, {name}.f{i});")
    # Fold each FP var's exact bit pattern into the checksum.
    for name, ctype in g.fvars:
        helper = "fbits_f" if ctype == "float" else "fbits_d"
        main.append(f"  cs = csmix(cs, {helper}({name}));")

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
