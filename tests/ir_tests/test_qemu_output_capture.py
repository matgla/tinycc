"""SubprocessSUT must hand back every byte the guest wrote, however late it reads.

-nographic puts QEMU's stdout in O_NONBLOCK, and semihosting writes reach the
same open file through stderr, so a full pipe dropped whole lines while the guest
still exited 0.  bug_u64_zero_high_shift (30 KB of output) failed at random lines
once fs.pipe-user-pages-soft shrank new pipes to 8 KB.
"""
import time

import pytest

from qemu_run import CompileConfig, run_test

SRC = "bug_qemu_large_output.c"
LINES = 10000
MACHINE = "mps2-an505"


def _line(i):
    return f"line {i:05d} abcdefghijklmnopqrstuvwxyz012"


@pytest.fixture
def guest(tmp_path):
    suts = []

    def start(*defines):
        config = CompileConfig(extra_cflags="-O1", output_dir=tmp_path, defines=list(defines))
        sut, _ = run_test(SRC, MACHINE, config=config)
        suts.append(sut)
        return sut

    yield start
    for sut in suts:
        sut._proc.kill()
        sut.close()
        sut.logfile.close()


def test_every_line_survives_a_reader_that_starts_after_exit(guest):
    sut = guest()
    sut.wait(timeout=60)
    for i in range(LINES):
        sut.expect(rf"(?m)^{_line(i)}\n", timeout=5)
    sut.expect(r"(?m)^done\n", timeout=5)
    assert sut.exitstatus == 7


def test_read_remaining_returns_all_output(guest):
    sut = guest()
    sut.expect(rf"(?m)^{_line(0)}\n", timeout=30)
    sut.wait(timeout=60)
    lines = sut.read_remaining().splitlines()
    assert lines == [_line(i) for i in range(1, LINES)] + ["done"]


def test_output_is_readable_while_the_guest_runs(guest):
    sut = guest("HANG")
    sut.expect(r"(?m)^hanging\n", timeout=30)
    assert sut._proc.poll() is None


def test_silent_guest_hits_the_deadline(guest):
    sut = guest("HANG")
    start = time.monotonic()
    with pytest.raises(TimeoutError, match="Timeout waiting"):
        sut.expect("never printed", timeout=1)
    assert time.monotonic() - start < 10


def test_endless_output_still_hits_the_deadline(guest):
    sut = guest("SPAM")
    start = time.monotonic()
    with pytest.raises(TimeoutError, match="Timeout waiting"):
        sut.expect("never printed", timeout=1)
    assert time.monotonic() - start < 10


def test_missing_line_after_exit_fails_at_once(guest):
    sut = guest()
    sut.wait(timeout=60)
    start = time.monotonic()
    with pytest.raises(TimeoutError, match="before process exit"):
        sut.expect("never printed", timeout=60)
    assert time.monotonic() - start < 30


def test_close_stops_a_running_guest_and_is_repeatable(guest):
    sut = guest("HANG")
    sut.close()
    sut.close()
    assert sut._proc.poll() is not None
