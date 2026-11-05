"""Comments and skipped lines end where they should, wherever the I/O buffer ends.

parse_comment and skip_logical_line search the buffer with memchr, each search
stopping at what the one before it found.  What can go wrong is at the edges: a
'*', a '/', a backslash or a newline that is the last byte of one read or the
first of the next, and a line longer than the buffer.  2048 is the device's
IO_BUF_SIZE, 8192 the host's; both are covered so the file means the same to
the cross and to the compiler running on the board.
"""

import re
import subprocess

import pytest

from conftest import _find_compiler


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


# Each ends the comment or the skipped region exactly once, then says so.
CASES = {
    "block": "/* one\n two\n * three **\n*/ int after;\n",
    "star_slash_split": "/* one *\\\n/ int after;\n",
    "star_backslash_text": "/* one \\ two *\\ three\n*/ int after;\n",
    "only_stars": "/****/ int after;\n",
    "continued_newline": "/* one \\\n two \\\n*/ int after;\n",
    "skipped_line": "#if 0\nskipped\n#endif\nint after;\n",
    "skipped_continuation": "#if 0\nskipped \\\n#endif\nskipped\n#endif\nint after;\n",
    "skipped_star": "#if 0\nskipped * / * \\ x\n#endif\nint after;\n",
    "long_comment": "/* " + "x" * 9000 + " */ int after;\n",
    "long_comment_star": "/* " + "*" * 9000 + "/ int after;\n",
    "long_skipped_line": "#if 0\n" + "skipped " * 1200 + "\n#endif\nint after;\n",
}


def preprocess(tcc, tmp_path, source):
    src = tmp_path / "t.c"
    src.write_bytes(source.encode())
    r = subprocess.run([tcc, "-E", "-P", "-nostdinc", str(src)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout


@pytest.mark.parametrize("case", CASES)
@pytest.mark.parametrize("buffer_end", [2048, 8192])
def test_sequence_at_the_buffer_end(tcc, tmp_path, case, buffer_end):
    text = CASES[case]
    for offset in range(-24, 6):
        pad = buffer_end + offset - len("/**/\n")
        source = "/*" + " " * pad + "*/\n" + text + "int line = __LINE__;\n"
        out = preprocess(tcc, tmp_path, source)
        assert re.findall(r"\bafter\b", out) == ["after"], (offset, out[:200])
        assert "skipped" not in out, offset
        assert f"int line = {source.count(chr(10))};" in out, (offset, out[-120:])


def test_unterminated_comment_is_still_an_error(tcc, tmp_path):
    src = tmp_path / "t.c"
    src.write_text("/* never closed\n * nor here\n")
    r = subprocess.run([tcc, "-E", "-nostdinc", str(src)], capture_output=True, text=True)
    assert r.returncode != 0
    assert "unexpected end of file in comment" in r.stderr
