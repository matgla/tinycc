"""yasc `unreachable` and the missing-return check (-Wreturn-type).

`unreachable` is a void expression that traps when reached (`udf #254`, as
the -fsanitize checks): the branch into it is the check, and every -O level
keeps it.  Code after it is off, as after a return, so a non-void function
may end in one; `catch unreachable` says a call cannot fail, and
`default: unreachable` leaves -Wswitch looking for the enum's missing cases.
Handler mode (-fsanitize=unreachable -fno-sanitize-trap) reports the place
through __ubsan_handle_builtin_unreachable and aborts.  Outside yasc it is an
identifier.  __builtin_unreachable() stays an assumption with no code unless
-fsanitize=unreachable (part of -fsanitize=undefined, as clang's) checks it.

The missing-return check reads the frontend's reachability: a path that falls
off the end of a non-void function is an error under the safe default.  An
infinite loop with no break, a noreturn call and __builtin_trap end a path.
"""

import re
import subprocess
from pathlib import Path

import pytest

from qemu_run import CompileConfig, build_qemu_command, compile_testcase

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
MACHINE = "mps2-an505"
LEVELS = ["-O0", "-O1", "-O2", "-Os"]
CASES = CURRENT_DIR / "yasc_unreachable_cases.c"
CASES_SRC = CASES.read_text().splitlines()

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = "#pragma yasc\nvoid note(int);\nvoid stop(void) __attribute__((noreturn));\n"


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-Wall", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _yasc(tmp_path, body, *flags):
    return _compile(tmp_path, PRELUDE + body, *flags)


def _clean(proc):
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def _errors(proc):
    return [line.split("error: ", 1)[1] for line in proc.stderr.splitlines() if "error: " in line]


def _warnings(proc):
    return [line.split("warning: ", 1)[1] for line in proc.stderr.splitlines() if "warning: " in line]


def _disasm(tmp_path, body, *flags):
    proc = _yasc(tmp_path, body, *flags)
    assert proc.returncode == 0, proc.stderr
    dis = subprocess.run(["arm-none-eabi-objdump", "-d", str(tmp_path / "t.o")], capture_output=True, text=True)
    assert dis.returncode == 0, dis.stderr
    return dis.stdout


# ------------------------------------------------------------- the keyword


def test_identifier_outside_yasc(tmp_path):
    proc = _compile(tmp_path, "int unreachable = 1;\nint f(int x) { return unreachable + x; }\n", "-std=c23")
    _clean(proc)


def test_a_name_in_scope_wins(tmp_path):
    _clean(_yasc(tmp_path, "int f(int unreachable) { return unreachable + 1; }\n"))


@pytest.mark.parametrize("opt", LEVELS)
def test_ends_a_non_void_function(tmp_path, opt):
    body = """
enum color { RED, GREEN, BLUE };
error E { Bad };
!int get(int x);
int half(int x) { if (x % 2 == 0) return x / 2; unreachable; }
int name(enum color c) { switch (c) { case RED: return 1; case GREEN: return 2; case BLUE: return 3; default: unreachable; } }
int pick(int x) { return get(x) catch unreachable; }
int pick2(int x) { int v = get(x) catch stop(); return v; }
int nested(int x) { if (x) { return 1; } else { unreachable; } }
void plain(int x) { if (x) unreachable; note(x); }
"""
    _clean(_yasc(tmp_path, body, opt))


@pytest.mark.parametrize("opt", LEVELS)
def test_traps_at_every_level(tmp_path, opt):
    # the branch to the trap is the check: no level folds it away
    dis = _disasm(tmp_path, "int half(int x) { if (x % 2 == 0) return x / 2; unreachable; }\n", opt)
    assert dis.count("udf\t#254") == 1, dis


def test_builtin_unreachable_stays_unchecked(tmp_path):
    body = "int f(int x) { if (x > 1) return x; __builtin_unreachable(); }\n"
    for opt in LEVELS:
        dis = _disasm(tmp_path, body, opt)
        assert "udf" not in dis, (opt, dis)
        dis = _disasm(tmp_path, body, opt, "-fsanitize=unreachable")
        assert dis.count("udf\t#254") == 1, (opt, dis)
        dis = _disasm(tmp_path, body, opt, "-fsanitize=undefined")
        assert dis.count("udf\t#254") == 1, (opt, dis)


def test_handler_mode_calls_the_runtime(tmp_path):
    dis = _disasm(tmp_path, "int f(int x) { if (x) return x; unreachable; }\n", "-fsanitize=unreachable",
                  "-fno-sanitize-trap")
    assert "__ubsan_handle_builtin_unreachable" in subprocess.run(
        ["arm-none-eabi-nm", str(tmp_path / "t.o")], capture_output=True, text=True).stdout
    assert dis.count("udf\t#254") == 1, dis
    # without -fsanitize=unreachable it is a plain trap, no runtime needed
    _disasm(tmp_path, "int f(int x) { if (x) return x; unreachable; }\n", "-fno-sanitize-trap")
    nm = subprocess.run(["arm-none-eabi-nm", str(tmp_path / "t.o")], capture_output=True, text=True).stdout
    assert "__ubsan" not in nm, nm


def test_default_unreachable_keeps_switch_check(tmp_path):
    body = """
enum color { RED, GREEN, BLUE };
int a(enum color c) { switch (c) { case RED: return 1; case GREEN: return 2; default: unreachable; } }
int b(enum color c) { switch (c) { case RED: return 1; case GREEN: return 2; default: return 0; } }
"""
    proc = _yasc(tmp_path, body, "-Wswitch")
    assert proc.returncode == 0, proc.stderr
    assert _warnings(proc) == ["enumeration value 'BLUE' not handled in switch"], proc.stderr


def test_value_use_is_an_error(tmp_path):
    proc = _yasc(tmp_path, "int f(void) { int x = unreachable; return x; }\n")
    assert proc.returncode != 0 and _errors(proc), proc.stderr


# ------------------------------------------------------- missing return

MISSING = {
    "if_without_else": "int f(int x) { if (x) return 1; }",
    "loop_with_break": "int f(int x) { for (;;) { if (x) break; } }",
    "enum_switch_no_default": "enum c { R, G }; int f(enum c v) { switch (v) { case R: return 1; case G: return 2; } }",
    "while_cond": "int f(int x) { while (x) { return 1; } }",
    "switch_default_breaks": "int f(int x) { switch (x) { case 1: return 1; default: break; } }",
    "do_while_cond": "int f(int x) { do { if (x) return 1; } while (x > 3); }",
    "while1_with_break": "int f(int x) { while (1) { if (x) break; } }",
    "do_while1_with_break": "int f(int x) { do { if (x) break; } while (1); }",
}

COMPLETE = {
    "if_else": "int f(int x) { if (x) return 1; else return 2; }",
    "while1": "int f(int x) { while (1) { if (x) return 1; } }",
    "for_ever": "int f(int x) { for (;;) { if (x) return 1; note(x); } }",
    "do_while1": "int f(int x) { do { if (x) return 1; } while (1); }",
    "do_return": "int f(int x) { do { return 1; } while (x); }",
    "noreturn_call": "int f(int x) { if (x) return 1; stop(); }",
    "builtin_trap": "int f(int x) { if (x) return 1; __builtin_trap(); }",
    "builtin_unreachable": "int f(int x) { if (x) return 1; __builtin_unreachable(); }",
    "switch_all_return": "int f(int x) { switch (x) { case 1: return 1; default: return 2; } }",
    "goto_label": "int f(int x) { goto done; done: return x; }",
    "break_in_inner_switch": "int f(int x) { for (;;) { switch (x) { case 1: break; } if (x > 2) return x; } }",
    "unreachable": "int f(int x) { if (x) return 1; unreachable; }",
}


def _safe_default(tmp_path, body):
    """tcc's safe default: yasc, and -Wreturn-type is an error"""
    src = tmp_path / "s.c"
    src.write_text("void note(int);\nvoid stop(void) __attribute__((noreturn));\n" + body)
    return subprocess.run([str(TCC), "-Wno-missing-prototypes", "-c", str(src), "-o", str(tmp_path / "s.o")],
                          capture_output=True, text=True)


MSG = "function might return no value: 'f'"


@pytest.mark.parametrize("name", sorted(MISSING))
def test_missing_return_is_reported(tmp_path, name):
    assert _errors(_safe_default(tmp_path, MISSING[name] + "\n")) == [MSG]
    # yasc by #pragma under -ccompat, and regular C: a warning
    for proc in (_yasc(tmp_path, MISSING[name] + "\n"), _compile(tmp_path, "void note(int);\n" + MISSING[name] + "\n")):
        assert proc.returncode == 0, proc.stderr
        assert _warnings(proc) == [MSG], proc.stderr


@pytest.mark.parametrize("name", sorted(COMPLETE))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_every_path_returns(tmp_path, name, opt):
    _clean(_yasc(tmp_path, COMPLETE[name] + "\n", opt))
    proc = _safe_default(tmp_path, COMPLETE[name] + "\n")
    assert proc.returncode == 0 and MSG not in proc.stderr, proc.stderr


def test_code_after_an_infinite_loop_is_not_generated(tmp_path):
    # the frontend's reachability, not a pass: the dead call never reaches the IR
    dis = _disasm(tmp_path, "void f(int x) { while (1) { if (x) return; } note(1); }\n", "-O0")
    assert "note" not in subprocess.run(["arm-none-eabi-nm", str(tmp_path / "t.o")], capture_output=True,
                                        text=True).stdout, dis


def test_label_after_an_infinite_loop_is_reached(tmp_path):
    body = "int f(int x) { if (x) goto out2; while (1) { note(x); } out2: return x; }\n"
    _clean(_yasc(tmp_path, body))


# ------------------------------------------------------------ at run time


def _line(tag):
    return [i + 1 for i, l in enumerate(CASES_SRC) if f"/* LINE {tag} */" in l][0]


def _runtime(tmp_path):
    out = tmp_path / "ubsan_runtime.o"
    if not out.exists():
        from cross_includes import INCLUDE_FLAGS
        proc = subprocess.run([str(TCC), "-ccompat", "-Wall", "-Werror", "-nostdlib", "-fno-pic", "-mno-sb-relative-got",
                               "-O1", "-c", *INCLUDE_FLAGS, str(CURRENT_DIR / "ubsan_runtime.c"), "-o", str(out)],
                              capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
    return out


def _build(tmp_path, flags, tag, handlers=False):
    cfg = CompileConfig(extra_cflags=flags, output_dir=tmp_path / "build", output_suffix=tag,
                        extra_objs=[_runtime(tmp_path)] if handlers else None)
    res = compile_testcase([CASES], MACHINE, config=cfg)
    assert res.success, res.error
    return res.elf_file


def _run(elf, case):
    proc = subprocess.run(build_qemu_command(MACHINE, elf, [case]), shell=True, capture_output=True, text=True,
                          timeout=60)
    out = proc.stdout + proc.stderr
    assert f"go {case}" in out, out
    return proc.returncode, out


TRAPS = {"switch": "switch", "half": "half", "catch": "catch", "loop": "loop", "inline": "inline"}
RUNS = {"switch_ok": 5, "half_ok": 2, "catch_ok": 7, "loop_ok": 30, "inline_ok": 4, "builtin_ok": 4}


@pytest.mark.parametrize("opt", LEVELS)
def test_runs_and_traps(tmp_path, opt):
    elf = _build(tmp_path, opt, "t" + opt.strip("-"))
    for case, value in RUNS.items():
        rc, out = _run(elf, case)
        assert rc == 0 and re.search(rf"(?m)^ok {case} {value}$", out), (case, out)
    for case in TRAPS:
        rc, out = _run(elf, case)
        # UNDEFINSTR: the UDF, not some other fault
        assert "HardFault" in out and "CFSR=0x00010000" in out and f"ok {case}" not in out, (case, out)


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_handler_reports_the_place(tmp_path, opt):
    elf = _build(tmp_path, f"{opt} -fsanitize=unreachable -fno-sanitize-trap", "h" + opt.strip("-"), handlers=True)
    for case, tag in list(TRAPS.items()) + [("builtin", "builtin")]:
        rc, out = _run(elf, case)
        reports = re.findall(r"(?m)^.*runtime error: .*$", out)
        assert rc != 0 and f"ok {case}" not in out and "HardFault" not in out, (case, out)
        assert len(reports) == 1 and re.fullmatch(
            rf".*yasc_unreachable_cases\.c:{_line(tag)}: runtime error: execution reached an unreachable program point",
            reports[0]), (case, out)
    rc, out = _run(elf, "half_ok")
    assert rc == 0 and "runtime error" not in out, out
