"""Include search: tccpp.c lists a directory instead of probing it per header.

The listing must be invisible except in cost, so these pin the search order and
every way a name can resolve, then count the failed opens themselves.
"""

import os
import shutil
import subprocess

import pytest

from conftest import _find_compiler


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def write(root, rel, text):
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def preprocess(tcc, root, source, *flags, env=None):
    src = write(root, "main.c", source)
    return subprocess.run([tcc, "-E", "-nostdinc", "-P", *flags, str(src)], capture_output=True, text=True,
                          cwd=root, env=env)


def test_first_directory_holding_the_header_wins(tcc, tmp_path):
    write(tmp_path, "a/other.h", "A_OTHER\n")
    write(tmp_path, "b/h.h", "FROM_B\n")
    write(tmp_path, "c/h.h", "FROM_C\n")
    r = preprocess(tcc, tmp_path, "#include <h.h>\n", "-Ia", "-Ib", "-Ic")
    assert r.returncode == 0, r.stderr
    assert "FROM_B" in r.stdout and "FROM_C" not in r.stdout


def test_subdirectory_name_is_looked_up_by_its_first_component(tcc, tmp_path):
    write(tmp_path, "a/sys/other.h", "WRONG\n")
    write(tmp_path, "b/sys/types.h", "FROM_B\n")
    r = preprocess(tcc, tmp_path, "#include <sys/types.h>\n", "-Ia", "-Ib")
    assert r.returncode == 0, r.stderr
    assert "FROM_B" in r.stdout


def test_dot_dot_in_a_name_reaches_outside_the_directory(tcc, tmp_path):
    write(tmp_path, "a/keep", "")
    write(tmp_path, "up.h", "FROM_UP\n")
    r = preprocess(tcc, tmp_path, '#include <../up.h>\n', "-Ia")
    assert r.returncode == 0, r.stderr
    assert "FROM_UP" in r.stdout


def test_include_next_continues_after_the_current_directory(tcc, tmp_path):
    write(tmp_path, "a/h.h", "FIRST\n#include_next <h.h>\n")
    write(tmp_path, "b/keep", "")
    write(tmp_path, "c/h.h", "SECOND\n#include_next <h.h>\n")
    write(tmp_path, "d/h.h", "THIRD\n")
    r = preprocess(tcc, tmp_path, "#include <h.h>\n", "-Ia", "-Ib", "-Ic", "-Id")
    assert r.returncode == 0, r.stderr
    assert r.stdout.split() == ["FIRST", "SECOND", "THIRD"]


def test_has_include_answers_both_ways(tcc, tmp_path):
    write(tmp_path, "b/there.h", "\n")
    source = "#if __has_include(<there.h>)\nYES\n#endif\n#if !__has_include(<gone.h>)\nNO\n#endif\n"
    r = preprocess(tcc, tmp_path, source, "-Ia", "-Ib")
    assert r.returncode == 0, r.stderr
    assert r.stdout.split() == ["YES", "NO"]


def test_missing_and_non_directory_paths_are_passed_over(tcc, tmp_path):
    write(tmp_path, "file", "not a directory\n")
    write(tmp_path, "b/h.h", "FROM_B\n")
    r = preprocess(tcc, tmp_path, "#include <h.h>\n", "-Inowhere", "-Ifile", "-Ib")
    assert r.returncode == 0, r.stderr
    assert "FROM_B" in r.stdout


def test_a_header_nowhere_is_still_an_error(tcc, tmp_path):
    write(tmp_path, "a/keep", "")
    r = preprocess(tcc, tmp_path, "#include <gone.h>\n", "-Ia")
    assert r.returncode != 0
    assert "include file 'gone.h' not found" in r.stderr


def test_symlink_to_a_header_is_found(tcc, tmp_path):
    write(tmp_path, "real/h.h", "THROUGH_LINK\n")
    (tmp_path / "a").mkdir()
    os.symlink("../real/h.h", tmp_path / "a/h.h")
    r = preprocess(tcc, tmp_path, "#include <h.h>\n", "-Ia")
    assert r.returncode == 0, r.stderr
    assert "THROUGH_LINK" in r.stdout


def test_quoted_include_prefers_the_including_files_directory(tcc, tmp_path):
    write(tmp_path, "sub/top.h", '#include "near.h"\n')
    write(tmp_path, "sub/near.h", "NEAR\n")
    write(tmp_path, "a/near.h", "FAR\n")
    r = preprocess(tcc, tmp_path, '#include "sub/top.h"\n', "-Ia")
    assert r.returncode == 0, r.stderr
    assert "NEAR" in r.stdout and "FAR" not in r.stdout


OPEN_LOG = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int logged(const char *symbol, const char *path, int flags, int mode) {
  int (*real)(const char *, int, ...) = dlsym(RTLD_NEXT, symbol);
  int result = real(path, flags, mode);
  const char *log = getenv("OPEN_LOG");
  if (result < 0 && log) {
    int fd = real(log, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { dprintf(fd, "%s\n", path); close(fd); }
  }
  return result;
}
int open(const char *p, int f, ...) { va_list a; va_start(a, f); int m = va_arg(a, int); va_end(a); return logged("open", p, f, m); }
int open64(const char *p, int f, ...) { va_list a; va_start(a, f); int m = va_arg(a, int); va_end(a); return logged("open64", p, f, m); }
"""


def test_directories_without_the_header_are_not_probed(tcc, tmp_path):
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("needs a host C compiler for the open() shim")
    shim_c = write(tmp_path, "shim/log.c", OPEN_LOG)
    shim = tmp_path / "shim/log.so"
    subprocess.run([cc, "-shared", "-fPIC", str(shim_c), "-o", str(shim), "-ldl"], check=True)

    flags = []
    for n in range(20):
        write(tmp_path, f"inc{n}/unrelated{n}.h", "\n")
        flags.append(f"-Iinc{n}")
    write(tmp_path, "last/h.h", "FOUND\n")
    flags.append("-Ilast")

    log = tmp_path / "failed_opens"
    env = dict(os.environ, LD_PRELOAD=str(shim), OPEN_LOG=str(log))
    # An ASan-built compiler (the CI configure) refuses to start when something
    # other than its runtime heads LD_PRELOAD; the shim only logs failed opens.
    env["ASAN_OPTIONS"] = ":".join(filter(None, [env.get("ASAN_OPTIONS"), "verify_asan_link_order=0"]))
    r = preprocess(tcc, tmp_path, "#include <h.h>\n#include <h.h>\n", *flags, env=env)
    assert r.returncode == 0, r.stderr
    assert "FOUND" in r.stdout
    failed = [line for line in (log.read_text().splitlines() if log.exists() else []) if line.endswith("h.h")]
    assert failed == [], f"{len(failed)} failed opens: {failed[:5]}"
