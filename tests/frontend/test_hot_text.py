"""--hot-text: the listed functions are the last block of .text, and calls in and
out of the block go through veneers (arm-link.c).  scripts in yasos run the result:
tests/smoke/hot_text_test.py, and the device's own tcc is linked this way.
"""

import re
import subprocess
from pathlib import Path

import pytest

from conftest import _find_compiler

SOURCE = """
int cold_a(int a) { return a + 1; }
int cold_b(int a) { return hot_a(a) + cold_a(a); }
int hot_b(int a) { return a * 3; }
int hot_a(int a) { return hot_b(a) + cold_a(a); }
int cold_far(int a);
int hot_tail(int a) { return cold_far(a); }
int _start(int a) { return cold_b(a) + hot_tail(a); }
void padding(void) { __asm__(".space 1200000"); }
int cold_far(int a) { return a + 2; }
"""

SLOT_TEXT, SLOT_HOT = 32, 40  # GOT index 4 and 5, 8 bytes each


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def link(tcc, tmp_path, hot=None, name="t"):
    root = Path(tcc).parent
    src = tmp_path / "t.c"
    src.write_text("int hot_a(int);\n" + SOURCE)
    exe = tmp_path / name
    flags = [f"-Wl,--hot-text={hot}"] if hot else []
    r = subprocess.run([tcc, f"-B{root}", f"-L{root}", f"-L{root}/lib", "-g", "-O0", "-nostdlib", "-static", "-ffunction-sections",
                        "-mpic-data-is-text-relative", "-Wl,-oformat=elf32-littlearm", "-Wl,-Ttext=0x0",
                        "-Wl,-e,_start", *flags, str(src), "-o", str(exe)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return exe


def symbols(exe):
    out = subprocess.run(["arm-none-eabi-nm", str(exe)], capture_output=True, text=True, check=True).stdout
    return {p[2]: int(p[0], 16) & ~1 for p in (l.split() for l in out.splitlines()) if len(p) == 3}


def disassembly(exe):
    return subprocess.run(["arm-none-eabi-objdump", "-d", "--no-show-raw-insn", str(exe)],
                          capture_output=True, text=True, check=True).stdout


def body(dis, name):
    m = re.search(rf"^[0-9a-f]+ <{name}>:\n((?:\s+[0-9a-f]+:.*\n)+)", dis, re.M)
    assert m, name
    return m[1]


def veneer_target(text):
    """(GOT slot, offset the veneer adds to that slot's base)."""
    slot = re.search(r"ldr\.w\s+ip, \[r9, #(\d+)\]", text)
    assert slot, text
    off = sum(int(v, 0) for v in re.findall(r"add[w.w]*\s+ip, ip?,? ?#(\d+)", text))
    assert "bx\tip" in text, text
    return int(slot[1]), off


def test_listed_functions_are_the_last_block(tcc, tmp_path):
    hot = tmp_path / "hot.txt"
    hot.write_text("# a comment\nhot_a\nhot_b  # another\nhot_tail\nnot_a_function\n")
    s = symbols(link(tcc, tmp_path, hot))
    block = [s["hot_a"], s["hot_b"], s["hot_tail"]]
    rest = [s["cold_a"], s["cold_b"], s["_start"]]
    assert min(block) > max(rest)


def test_a_call_inside_the_block_stays_direct_and_one_across_gets_a_veneer(tcc, tmp_path):
    hot = tmp_path / "hot.txt"
    hot.write_text("hot_a\nhot_b\nhot_tail\n")
    exe = link(tcc, tmp_path, hot)
    dis = disassembly(exe)
    a = body(dis, "hot_a")
    assert re.search(r"bl\s+[0-9a-f]+ <hot_b>", a), a
    assert not re.search(r"bl\s+[0-9a-f]+ <cold_a>", a), a
    assert not re.search(r"bl\s+[0-9a-f]+ <hot_a>", body(dis, "cold_b"))
    assert re.search(r"bl\s+[0-9a-f]+ <cold_a>", body(dis, "cold_b"))


def test_each_veneer_adds_the_distance_to_its_target(tcc, tmp_path):
    hot = tmp_path / "hot.txt"
    hot.write_text("hot_a\nhot_b\nhot_tail\n")
    exe = link(tcc, tmp_path, hot)
    s = symbols(exe)
    dis = disassembly(exe)
    seen, targets = set(), set()
    block_start = min(s["hot_a"], s["hot_b"], s["hot_tail"])
    for name, addr in s.items():
        if not name.startswith("__tcc_hot_veneer_"):
            continue
        slot, off = veneer_target(body(dis, name))
        seen.add(slot)
        if slot == SLOT_TEXT:
            # from the block out: inside the block, to something that is not
            assert addr >= block_start
            assert off in (s["cold_a"], s["cold_far"])
            targets.add(off)
        else:
            assert slot == SLOT_HOT
            assert addr >= block_start  # after the whole of the rest: .text's end
            assert block_start + off in (s["hot_a"], s["hot_tail"])
    assert seen == {SLOT_TEXT, SLOT_HOT}
    # bits 12-19 and 20-27 of the offset: the distance has to need them
    assert targets == {s["cold_a"], s["cold_far"]} and s["cold_far"] > 1 << 20 and s["cold_far"] & 0xff000


def test_one_veneer_per_side_and_target(tcc, tmp_path):
    hot = tmp_path / "hot.txt"
    hot.write_text("hot_a\nhot_b\nhot_tail\n")
    s = symbols(link(tcc, tmp_path, hot))
    # hot_a to cold_a, hot_tail to cold_far; cold_b to hot_a, _start to hot_tail.
    assert sum(n.startswith("__tcc_hot_veneer_") for n in s) == 4


def test_an_empty_list_changes_nothing(tcc, tmp_path):
    hot = tmp_path / "hot.txt"
    hot.write_text("# nothing\nnot_a_function\n")
    assert link(tcc, tmp_path, hot, "a").read_bytes() == link(tcc, tmp_path, None, "b").read_bytes()


def compile_objects(tcc, tmp_path, sources):
    root = Path(tcc).parent
    objs = []
    for name, text in sources.items():
        src = tmp_path / f"{name}.c"
        src.write_text(text)
        obj = tmp_path / f"{name}.o"
        r = subprocess.run([tcc, f"-B{root}", "-g", "-O0", "-ffunction-sections", "-mpic-data-is-text-relative",
                            "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        objs.append(str(obj))
    return objs


def link_objects(tcc, tmp_path, objs, *flags, name="o"):
    root = Path(tcc).parent
    exe = tmp_path / name
    r = subprocess.run([tcc, f"-B{root}", f"-L{root}", f"-L{root}/lib", "-g", "-nostdlib", "-static",
                        "-mpic-data-is-text-relative", "-Wl,-oformat=elf32-littlearm", "-Wl,-Ttext=0x0",
                        "-Wl,-e,_start", *flags, *objs, "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return exe


def test_objects_keep_their_sections_for_the_list_without_gc(tcc, tmp_path):
    """A link from objects folds .text.<fn> into .text as it loads them, unless GC or
    the list needs them: libc.so has no --gc-sections, and got no block at all."""
    objs = compile_objects(tcc, tmp_path, {"a": "int hot_a(int a) { return a + 1; }\nint cold_a(int a) { return a + 2; }\n",
                                           "b": "int hot_a(int); int cold_a(int);\nint _start(int a) { return hot_a(a) + cold_a(a); }\n"})
    hot = tmp_path / "hot.txt"
    hot.write_text("hot_a\n")
    s = symbols(link_objects(tcc, tmp_path, objs, f"-Wl,--hot-text={hot}"))
    assert s["hot_a"] > s["cold_a"] and s["hot_a"] > s["_start"]
    assert any(n.startswith("__tcc_hot_veneer_") for n in s)


def sections_of(exe):
    out = subprocess.run(["arm-none-eabi-readelf", "-SW", str(exe)], capture_output=True, text=True, check=True).stdout
    return set(re.findall(r"\]\s+(\S+)\s+PROGBITS", out))


def test_ram_text_moves_listed_functions_to_time_critical(tcc, tmp_path):
    """Zig's C backend numbers its functions differently on every change: the list has no number."""
    objs = compile_objects(tcc, tmp_path, {"k": "int fn_a__1234(int a) { return a + 1; }\n"
                                                "int fn_b(int a) { return a + 2; }\n"
                                                "int fn_a__x12(int a) { return a + 3; }\n"
                                                "int other(int a) { return a + 4; }\n"
                                                "int _start(int a) { return fn_a__1234(a) + fn_b(a) + fn_a__x12(a) + other(a); }\n"})
    ram = tmp_path / "ram.txt"
    ram.write_text("# hot\nfn_a\nfn_b\n")
    names = sections_of(link_objects(tcc, tmp_path, objs, f"-Wl,--ram-text={ram}"))
    assert ".time_critical.fn_a__1234" in names
    assert ".time_critical.fn_b" in names
    assert ".time_critical.other" not in names
    assert ".time_critical.fn_a__x12" not in names, "only digits after __ are a number"


def test_a_missing_list_is_an_error(tcc, tmp_path):
    objs = compile_objects(tcc, tmp_path, {"k": "int _start(int a) { return a; }\n"})
    root = Path(tcc).parent
    r = subprocess.run([tcc, f"-B{root}", f"-L{root}", f"-L{root}/lib", "-nostdlib", "-static", "-mpic-data-is-text-relative",
                        "-Wl,-Ttext=0x0", "-Wl,-e,_start", f"-Wl,--ram-text={tmp_path}/nope", *objs, "-o", str(tmp_path / "x")],
                       capture_output=True, text=True)
    assert r.returncode != 0 and "cannot read" in r.stderr


def test_a_flash_caller_calls_the_flash_copy_of_a_marked_function(tcc, tmp_path):
    hot = tmp_path / "hot.txt"
    hot.write_text("=hot_a\nhot_b\nhot_tail\n")
    exe = link(tcc, tmp_path, hot)
    s = symbols(exe)
    dis = disassembly(exe)
    assert min(s["hot_a"], s["hot_b"], s["hot_tail"]) > s["cold_b"], "marked or not, it is in the block"
    assert re.search(r"bl\s+[0-9a-f]+ <hot_a>", body(dis, "cold_b")), "no veneer from flash"
    assert not re.search(r"bl\s+[0-9a-f]+ <hot_tail>", body(dis, "_start")), "unmarked: still a veneer"
    assert re.search(r"bl\s+[0-9a-f]+ <hot_b>", body(dis, "hot_a"))


def test_the_list_names_zig_functions_without_their_numbers(tcc, tmp_path):
    """Zig's C backend numbers functions (fn__1234) and inner functions (fn__func_12__345)
    afresh on every change of the compiler: a list made from a profile names them without
    the numbers, as --ram-text does.  Only __<digits> ending the name or before '_' is a
    number: _28..._29 is how the backend spells '(...)'."""
    objs = compile_objects(tcc, tmp_path, {"k": "int fn_a__1234(int a) { return a + 1; }\n"
                                                "int fn_g__func_7__88(int a) { return a + 2; }\n"
                                                "int pool_28256_29_bit__9(int a) { return a + 3; }\n"
                                                "int fn_a__x12(int a) { return a + 4; }\n"
                                                "int exact__77(int a) { return a + 5; }\n"
                                                "int cold__5(int a) { return a + 6; }\n"
                                                "int _start(int a) { return fn_a__1234(a) + fn_g__func_7__88(a) + "
                                                "pool_28256_29_bit__9(a) + fn_a__x12(a) + exact__77(a) + cold__5(a); }\n"})
    hot = tmp_path / "hot.txt"
    hot.write_text("fn_a\nfn_g\npool_28256_29_bit\nexact__77\n")
    s = symbols(link_objects(tcc, tmp_path, objs, f"-Wl,--hot-text={hot}"))
    cold = max(s["cold__5"], s["_start"], s["fn_a__x12"])
    for name in ("fn_a__1234", "fn_g__func_7__88", "pool_28256_29_bit__9", "exact__77"):
        assert s[name] > cold, name
