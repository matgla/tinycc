#!/usr/bin/env python3
"""The worst-case stack of a program, from tcc's per-object stack files.

Compile with

    armv8m-tcc -c -fstack-usage -fcallgraph-info=su x.c -o x.o   # x.su, x.ci

and give this script the .ci files (or the directories holding them).  For
each entry point (`main` unless -e names others) it follows every path through
the direct calls and adds the frames up:

    scripts/stack_usage.py build/                 # main, every .ci below build/
    scripts/stack_usage.py -e syscall_entry -e irq_handler kernel/*.ci
    scripts/stack_usage.py build/ --unknown-budget 256 --budget memcpy=16
    scripts/stack_usage.py build/ --limit 4096    # exit 1 above 4096 bytes
    scripts/stack_usage.py build/ --libtcc1 ../libc/build/libc.a   # with the runtimes' .ci

The runtimes' .ci stay in their build trees (armv8m-libtcc1.ci beside
armv8m-libtcc1.a here, libc.ci in libc's build/): host-side data, never
installed into a target's image.

A path is UNBOUNDED when it recurses or passes a frame whose size is only
known at run time (VLA, alloca: "dynamic" in the .su).  A call to a function
no .ci describes (libc, libtcc1, assembly) is UNKNOWN: the sum is then a lower
bound, unless --unknown-budget gives every such callee a fixed cost (or
--budget NAME=BYTES one of them).  A tail call (B.W) leaves the caller's frame
before the callee runs, so it costs the callee alone.

A call through a pointer goes to one of the functions whose address is taken
somewhere in the files given and whose type has the call's calling shape
(below): the worst of them is the call's cost, so with every object of the
program described the path stays bounded.  None of that shape: UNKNOWN, as a
callee no file describes.  The call's type not known (tcc did not see it):
UNBOUNDED.  --indirect-unbounded makes every call through a pointer UNBOUNDED
(the old behaviour); --indirect-budget BYTES then gives each a fixed cost
instead, as it does for the calls of no known type and of no candidate.

What the resolution cannot see (it is sound only without these):
  - a function pointer cast to a type of another calling shape and called
    through it (the shape is the ABI's: every pointer, integer up to 32 bits,
    enum and _Bool is one class, so `int (*)(const void *, const void *)`
    reaching `int cmp(const struct s *, const struct s *)` is covered);
  - an address taken in assembly, in a unit not given (no .ci: a library's
    callbacks into it, a pointer made in another program), or computed
    (an address read from a table not relocated against the function);
  - calls through a register in inline or separate assembly (not counted).

Plain .su files (gcc's, or tcc's without -fcallgraph-info) carry no callees:
they are listed by frame size (--top) but no path can be summed through them.

The .ci format (one per object, written by tcc -fcallgraph-info):

    # tcc callgraph-info 2
    function NAME FILE:LINE BYTES QUALIFIER [local]
    type SIG                   its calling shape
    call CALLEE                a BL
    tailcall CALLEE            a B.W
    indirect N SIG             N calls through a register (BLX) of shape SIG
    indirect-tailcall N SIG    N tail calls through a register (BX)
    address-taken NAME [local] (after the functions) a function this object
                               refers to other than by a branch
    object NAME                in an archive's .ci (armv8m-libtcc1.ci beside
                               armv8m-libtcc1.a): the member the lines after
                               it describe

SIG is "RET(ARG,...)" in classes: i (pointer, integer up to 32 bits, enum,
_Bool), l (64-bit integer), f, d, v (void), sN (struct or union of N bytes),
cN (complex), xN (vector); `...` for a variadic one, "RET(?)" for a type with
no prototype (it matches any of the same RET), "?" when the type is not known.
Version 1 files (no types, no address-taken lines) make every indirect call
UNBOUNDED.  QUALIFIER is gcc's: static, dynamic,bounded (SP moves inside the
body by a known amount, already in BYTES) or dynamic.  A `local` function
(static in C) is found only by calls from the same object.
"""

import argparse
import os
import sys
from dataclasses import dataclass, field

UNBOUNDED_FLAGS = ("recursion", "indirect", "dynamic")


@dataclass
class Func:
    name: str
    where: str
    bytes: int
    qual: str
    local: bool
    obj: str
    calls: list = field(default_factory=list)  # (callee, tail)
    indirect: int = 0
    indirect_tail: int = 0
    sig: str = None  # its own calling shape (None: a version 1 file)
    indirect_sigs: list = field(default_factory=list)  # (sig, tail)


@dataclass
class Result:
    bytes: int
    flags: set
    unknown: set
    path: list  # [(name, frame bytes, how)] from the entry down


def parse_ci(path, funcs, taken):
    """Returns the file's format version."""
    cur = None
    version = None
    obj = path
    with open(path) as fp:
        for raw in fp:
            line = raw.strip()
            if line.startswith("# tcc callgraph-info"):
                v = int(line.split()[-1])
                version = v if version is None else min(version, v)
                continue
            if not line or line.startswith("#"):
                continue
            words = line.split()
            kind = words[0]
            if kind == "object":  # an archive's .ci: one section per member
                obj, cur = f"{path}({words[1]})", None
            elif kind == "function":
                cur = Func(words[1], words[2], int(words[3]), words[4], "local" in words[5:], obj)
                funcs.append(cur)
            elif kind == "address-taken":
                taken.append((obj, words[1], "local" in words[2:]))
            elif cur is None:
                raise SystemExit(f"{path}: '{kind}' before any function")
            elif kind == "type":
                cur.sig = words[1]
            elif kind in ("call", "tailcall"):
                cur.calls.append((words[1], kind == "tailcall"))
            elif kind in ("indirect", "indirect-tailcall"):
                tail = kind == "indirect-tailcall"
                if tail:
                    cur.indirect_tail += int(words[1])
                else:
                    cur.indirect += int(words[1])
                cur.indirect_sigs.append((words[2] if len(words) > 2 else "?", tail))
            else:
                raise SystemExit(f"{path}: unknown line '{line}'")
    return version or 1


def parse_su(path, funcs):
    with open(path) as fp:
        for raw in fp:
            parts = raw.rstrip("\n").split("\t")
            if len(parts) != 3:
                continue
            loc, size, qual = parts
            where, _, name = loc.rpartition(":")
            funcs.append(Func(name, where, int(size), qual, False, path))


def collect(paths):
    ci, su = [], []
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for f in sorted(files):
                    full = os.path.join(root, f)
                    if f.endswith(".ci"):
                        ci.append(full)
                    elif f.endswith(".su"):
                        su.append(full)
        elif p.endswith(".ci"):
            ci.append(p)
        elif p.endswith(".a"):  # an archive: the .ci gathered beside it (libtcc1's)
            if not os.path.exists(p[:-2] + ".ci"):
                raise SystemExit(f"stack_usage: no {p[:-2]}.ci beside {p}")
            ci.append(p[:-2] + ".ci")
        elif p.endswith(".su"):
            su.append(p)
        else:
            raise SystemExit(f"stack_usage: not a .ci/.su file or a directory: {p}")
    funcs, taken, old = [], [], []
    for p in ci:
        if parse_ci(p, funcs, taken) < 2:
            old.append(p)
    # a .su beside a .ci says nothing more
    have = {os.path.splitext(p)[0] for p in ci}
    only_su = []
    for p in su:
        if os.path.splitext(p)[0] not in have:
            parse_su(p, only_su)
    return funcs, only_su, taken, old


def split_sig(sig):
    ret, _, rest = sig.partition("(")
    return ret, rest[:-1]


def sig_matches(call, fn):
    """May a call of shape CALL reach a function of shape FN?"""
    if fn is None:  # a version 1 record: no type, any call may reach it
        return True
    cr, ca = split_sig(call)
    fr, fa = split_sig(fn)
    return cr == fr and (ca == fa or ca == "?" or fa == "?")


class Graph:
    def __init__(self, funcs, args, taken=()):
        self.args = args
        self.taken = taken
        self.globals = {}
        self.locals = {}
        for f in funcs:
            if f.local:
                self.locals[(f.obj, f.name)] = f
            elif f.name in self.globals and self.globals[f.name].obj != f.obj:
                print(f"stack_usage: '{f.name}' defined in {self.globals[f.name].obj} and {f.obj}; using the first",
                      file=sys.stderr)
            else:
                self.globals.setdefault(f.name, f)
        self.memo = {}
        self.on_stack = set()
        self.targets_memo = {}

    def targets(self, sig):
        """The address-taken functions a call of shape SIG may reach: Func
        records, and the names no file describes."""
        if sig not in self.targets_memo:
            found, unknown, seen = [], [], set()
            for obj, name, local in self.taken:
                f = self.locals.get((obj, name)) if local else self.globals.get(name)
                if f is None and not local:
                    if name not in seen:
                        seen.add(name)
                        unknown.append(name)
                    continue
                if f is not None and id(f) not in seen and sig_matches(sig, f.sig):
                    seen.add(id(f))
                    found.append(f)
            self.targets_memo[sig] = (found, unknown)
        return self.targets_memo[sig]

    def resolve(self, caller, name):
        return self.locals.get((caller.obj, name)) or self.globals.get(name)

    def lookup(self, name):
        if name in self.globals:
            return self.globals[name]
        matches = [f for (o, n), f in self.locals.items() if n == name]
        return matches[0] if len(matches) == 1 else None

    def leaf_cost(self, name):
        """A callee with no record: (bytes, unknown?)"""
        if name in self.args.budget:
            return self.args.budget[name], False
        if self.args.unknown_budget is not None:
            return self.args.unknown_budget, False
        return 0, True

    def indirect_calls(self, f):
        """(sig, tail) per kind of indirect call of F; '?' when not known."""
        out = list(f.indirect_sigs)
        # a version 1 record: counts without types
        if not out:
            out = [("?", False)] * bool(f.indirect) + [("?", True)] * bool(f.indirect_tail)
        return out

    def worst(self, f):
        key = id(f)
        if key in self.memo:
            return self.memo[key]
        if key in self.on_stack:
            return Result(0, {"recursion"}, set(), [(f.name, f.bytes, "recursion")])
        self.on_stack.add(key)
        flags = set()
        if f.qual == "dynamic":
            flags.add("dynamic")
        unknown = set()
        best_call, best_tail = Result(0, set(), set(), []), Result(0, set(), set(), [])
        for name, tail in f.calls:
            g = self.resolve(f, name)
            if g is None:
                cost, unk = self.leaf_cost(name)
                r = Result(cost, set(), {name} if unk else set(), [(name, cost, "unknown" if unk else "budget")])
            else:
                r = self.worst(g)
            flags |= r.flags
            unknown |= r.unknown
            if tail:
                if r.bytes >= best_tail.bytes:
                    best_tail = r
            elif r.bytes >= best_call.bytes:
                best_call = r
        for sig, tail in self.indirect_calls(f):
            cands = []
            if sig != "?" and not self.args.indirect_unbounded:
                found, unknown_names = self.targets(sig)
                cands = [self.worst(g) for g in found]
                for name in unknown_names:
                    cost, unk = self.leaf_cost(name)
                    cands.append(Result(cost, set(), {name} if unk else set(),
                                        [(name, cost, "unknown" if unk else "budget")]))
                if not cands:
                    label = f"(indirect {sig})"
                    if self.args.indirect_budget is not None:
                        cost = self.args.indirect_budget
                        cands = [Result(cost, set(), set(), [(label, cost, "budget")])]
                    else:
                        cands = [Result(0, set(), {label}, [(label, 0, "unknown")])]
            elif self.args.indirect_budget is not None:
                cost = self.args.indirect_budget
                cands = [Result(cost, set(), set(), [("(indirect)", cost, "budget")])]
            else:
                flags.add("indirect")
                continue
            for r in cands:
                flags |= r.flags
                unknown |= r.unknown
                if tail and r.bytes >= best_tail.bytes:
                    best_tail = r
                elif not tail and r.bytes >= best_call.bytes:
                    best_call = r
        self.on_stack.discard(key)
        through_call = f.bytes + best_call.bytes
        if best_tail.bytes > through_call:
            res = Result(best_tail.bytes, flags, unknown, [(f.name, f.bytes, f.qual)] + best_tail.path)
        else:
            res = Result(through_call, flags, unknown, [(f.name, f.bytes, f.qual)] + best_call.path)
        self.memo[key] = res
        return res


def budget_arg(text):
    name, sep, value = text.partition("=")
    if not sep:
        raise argparse.ArgumentTypeError("--budget wants NAME=BYTES")
    return name, int(value, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help=".ci / .su files, directories, or archives (their .ci beside them)")
    ap.add_argument("--libtcc1", action="store_true",
                    help="add armv8m-libtcc1.ci, the runtime's (the tinycc build beside this script)")
    ap.add_argument("-e", "--entry", action="append", help="entry point (default: main); repeat for more")
    ap.add_argument("--unknown-budget", type=int, metavar="BYTES",
                    help="cost of a call to a function no file describes (default: unknown)")
    ap.add_argument("--budget", type=budget_arg, action="append", default=[], metavar="NAME=BYTES",
                    help="cost of one such function")
    ap.add_argument("--indirect-budget", type=int, metavar="BYTES",
                    help="cost of a call through a pointer whose targets are not known (default: unbounded)")
    ap.add_argument("--indirect-unbounded", action="store_true",
                    help="do not resolve calls through a pointer by type: each is unbounded (or --indirect-budget)")
    ap.add_argument("--limit", type=int, metavar="BYTES",
                    help="exit 1 when an entry needs more, is unbounded, or has unknown callees")
    ap.add_argument("--top", type=int, default=0, metavar="N", help="also list the N largest frames")
    ap.add_argument("-q", "--quiet", action="store_true", help="one line per entry, no path")
    args = ap.parse_args()
    args.budget = dict(args.budget)
    if args.libtcc1:
        lib = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "armv8m-libtcc1.ci")
        if not os.path.exists(lib):
            raise SystemExit(f"stack_usage: no {os.path.normpath(lib)} (build armv8m-libtcc1.a)")
        args.paths.append(os.path.normpath(lib))
    if not args.paths:
        ap.error("no .ci files or directories given")

    funcs, only_su, taken, old = collect(args.paths)
    if old and not args.indirect_unbounded:
        print(f"stack_usage: {len(old)} version 1 .ci file(s) (no types, no address-taken functions), e.g. {old[0]}: "
              "calls through a pointer are unbounded", file=sys.stderr)
        args.indirect_unbounded = True
    graph = Graph(funcs, args, taken)
    status = 0
    for entry in args.entry or ["main"]:
        f = graph.lookup(entry)
        if f is None:
            print(f"{entry}: not found in any .ci file")
            status = status or 2
            continue
        r = graph.worst(f)
        bad = [x for x in UNBOUNDED_FLAGS if x in r.flags]
        if bad:
            verdict = f"UNBOUNDED ({', '.join(bad)}); {r.bytes} bytes on the deepest bounded path"
        elif r.unknown:
            verdict = f">= {r.bytes} bytes (unknown: {', '.join(sorted(r.unknown))})"
        else:
            verdict = f"{r.bytes} bytes"
        print(f"{entry}: {verdict}")
        if not args.quiet:
            depth = 0
            for name, size, how in r.path:
                depth += size
                print(f"    {size:7d} {depth:7d}  {name}  [{how}]")
        if args.limit is not None and (bad or r.unknown or r.bytes > args.limit):
            status = 1
    if args.top:
        print(f"largest frames:")
        for f in sorted(funcs + only_su, key=lambda f: -f.bytes)[: args.top]:
            print(f"    {f.bytes:7d}  {f.name}  {f.qual}  {f.where}")
    return status


if __name__ == "__main__":
    sys.exit(main())
