"""A link folds every function's section into .text and frees it before .text is allocated.

Numbers are rewritten when those sections leave the table, and past the first
1024 symbols get_sym_attr is in another block.  Enough functions for both to
matter; bug_link_many_function_sections.c runs the result.
"""

import re
import subprocess
from pathlib import Path

import pytest

from conftest import _find_compiler

FUNCTIONS = 2500


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def search_paths(tcc):
    root = Path(tcc).parent
    return [f"-B{root}", f"-L{root}", f"-L{root}/lib"]


def source():
    lines = ["extern int ext_data; int ext_data = 5; const int ro[3] = {1, 2, 3}; int zero;"]
    for n in range(FUNCTIONS):
        # Every third function is reached by nothing.
        lines.append(f"int f{n}(int a) {{ return a + {n} + ro[{n % 3}] + zero; }}")
    kept = [n for n in range(FUNCTIONS) if n % 3]
    lines.append("int (*const table[])(int) = {" + ", ".join(f"f{n}" for n in kept[::2]) + "};")
    calls = " + ".join(f"f{n}(a)" for n in kept[1::2])
    lines.append(f"int _start(int a) {{ return table[a](a) + ext_data + {calls}; }}")
    return "\n".join(lines) + "\n", kept


def text_sections(tcc, tmp_path, src, *flags):
    exe = tmp_path / "t"
    r = subprocess.run([tcc, *search_paths(tcc), "-O1", "-nostdlib", "-static", "-ffunction-sections",
                        "-Wl,-oformat=elf32-littlearm", "-Wl,-Ttext=0x8000", "-Wl,-e,_start", *flags,
                        str(src), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    out = subprocess.run(["arm-none-eabi-readelf", "-SW", str(exe)], capture_output=True, text=True, check=True).stdout
    return {m[1]: int(m[2], 16) for m in re.finditer(r"\]\s+(\.text\S*)\s+\S+\s+\S+\s+\S+\s+([0-9a-f]+)", out)}


def test_functions_fold_into_text_and_collected_ones_go(tcc, tmp_path):
    src = tmp_path / "t.c"
    src.write_text(source()[0])
    collected = text_sections(tcc, tmp_path, src, "-Wl,--gc-sections")
    everything = text_sections(tcc, tmp_path, src)
    assert list(collected) == [".text"] and list(everything) == [".text"]
    assert collected[".text"] < 0.9 * everything[".text"]


def test_module_with_more_got_entries_than_one_attribute_block(tcc, tmp_path):
    text, _ = source()
    src, exe = tmp_path / "t.c", tmp_path / "m"
    src.write_text(text)
    r = subprocess.run([tcc, *search_paths(tcc), "-O1", "-nostdlib", "-fpie", "-fPIE", "-ffunction-sections", "-Wl,--gc-sections",
                        "-Wl,-e,_start", str(src), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert exe.stat().st_size > 20000


def compile_objects(tcc, tmp_path, count):
    """Objects whose static functions have the same names, so same-named sections."""
    objects = []
    for k in range(count):
        src, obj = tmp_path / f"s{k}.c", tmp_path / f"s{k}.o"
        body = "".join(f"static int g{n}(int a) {{ return a * {n + k} + {k}; }}\n" for n in range(1500))
        calls = " + ".join(f"g{n}(a)" for n in range(0, 1500, 7))
        src.write_text(body + f"int entry{k}(int a) {{ return {calls}; }}\n")
        r = subprocess.run([tcc, "-O0", "-nostdlib", "-ffunction-sections", "-c", str(src), "-o", str(obj)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        objects.append(str(obj))
    return objects


def test_same_named_sections_grow_out_of_the_arena(tcc, tmp_path):
    """A section holding one object's relocations, in the arena, gets another's."""
    objects = compile_objects(tcc, tmp_path, 3)
    start = tmp_path / "start.c"
    start.write_text("int entry0(int), entry1(int), entry2(int);\n"
                     "int _start(int a) { return entry0(a) + entry1(a) + entry2(a); }\n")
    out = tmp_path / "out"
    r = subprocess.run([tcc, *search_paths(tcc), "-O0", "-nostdlib", "-static", "-Wl,--gc-sections",
                        "-Wl,-oformat=elf32-littlearm", "-Wl,-e,_start", str(start), *objects, "-o", str(out)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert out.stat().st_size > 10000


def test_arena_output_matches_a_link_without_one(tcc, tmp_path):
    """The same link of the same objects: below the arena's size and above it."""
    small = tmp_path / "small.c"
    small.write_text("int _start(void) { return 3; }\n")
    out = tmp_path / "small"
    r = subprocess.run([tcc, *search_paths(tcc), "-nostdlib", "-static", "-Wl,-oformat=elf32-littlearm",
                        "-Wl,-e,_start", str(small), "-o", str(out)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    objects = compile_objects(tcc, tmp_path, 2)
    start = tmp_path / "start.c"
    start.write_text("int entry0(int), entry1(int);\nint _start(int a) { return entry0(a) + entry1(a); }\n")
    obj = tmp_path / "start.o"
    assert subprocess.run([tcc, "-O0", "-nostdlib", "-c", str(start), "-o", str(obj)]).returncode == 0
    links = []
    for name, inputs in (("direct", [str(obj), *objects]), ("archive", None)):
        if inputs is None:
            lib = tmp_path / "libs.a"
            assert subprocess.run([tcc, "-ar", "rcs", str(lib), *objects]).returncode == 0
            inputs = [str(obj), str(lib)]  # archive members do not count towards the arena
        out = tmp_path / name
        r = subprocess.run([tcc, *search_paths(tcc), "-nostdlib", "-static", "-Wl,--gc-sections",
                            "-Wl,-oformat=elf32-littlearm", "-Wl,-e,_start", *inputs, "-o", str(out)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        links.append(out.read_bytes())
    assert links[0] == links[1]
