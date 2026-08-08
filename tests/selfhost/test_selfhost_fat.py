"""FAT-drive native-vs-cross round-trip self-host gate.

These tests run only when the YasOS environment is available.  Each case is
compiled and executed twice:

1. with the cross compiler on the host and run under QEMU (reference), and
2. with the native compiler inside the YasOS guest via the FAT-drive harness.

Any divergence in exit code or stdout is a self-host regression.
"""

import pytest

from selfhost_runner import (
    GuestUnavailable,
    normalize_output,
    run_cross_reference,
    run_native_via_fat,
)

# Curated tests2 cases that exercise the compiler without requiring heavy
# runtime support inside the YasOS FAT image.  Each must have a .expect file.
# Paths are relative to tests/ir_tests/ because qemu_run.py builds from there.
from pathlib import Path

IR_TESTS_DIR = Path(__file__).parent.parent / "ir_tests"
SELFHOST_FAT_TESTS = [
    str(IR_TESTS_DIR / "../tests2/00_assignment.c"),
    str(IR_TESTS_DIR / "../tests2/04_for.c"),
    str(IR_TESTS_DIR / "../tests2/07_function.c"),
    str(IR_TESTS_DIR / "../tests2/14_if.c"),
    str(IR_TESTS_DIR / "../tests2/15_recursion.c"),
    str(IR_TESTS_DIR / "../tests2/21_char_array.c"),
    str(IR_TESTS_DIR / "../tests2/27_sizeof.c"),
    str(IR_TESTS_DIR / "../tests2/28_strings.c"),
]


def _id_from_path(test_file):
    return Path(test_file).stem


@pytest.mark.selfhost
@pytest.mark.selfhost_fat
@pytest.mark.parametrize("test_file", SELFHOST_FAT_TESTS, ids=_id_from_path)
def test_selfhost_fat_roundtrip(
    test_file, yasos_root, native_tcc, tmp_path
):
    if yasos_root is None:
        pytest.skip(
            "YasOS environment not detected; FAT-drive self-host gate requires "
            "the YasOS repository (scripts/qemu_fatdisk_run.py and "
            "zig-out/bin/yasos_kernel)."
        )
    if native_tcc is None:
        pytest.skip(
            "Native tcc binary not found; run build_rootfs.sh to build "
            "rootfs/usr/bin/tcc or libs/tinycc/bin/armv8m-tcc.elf."
        )

    cross_lines, cross_exit = run_cross_reference(
        test_file, tmp_path / "cross", timeout=20
    )
    try:
        native_lines, native_exit = run_native_via_fat(
            yasos_root,
            native_tcc,
            test_file,
            tmp_path / "native",
            timeout=60,
        )
    except GuestUnavailable as exc:
        # The guest never ran anything, so this says nothing about the
        # compiler.  Same class of miss as an absent YasOS checkout: skip.
        # (Rebuild the kernel for the machine the FAT harness boots --
        # `zig build defconfig -Ddefconfig_file=configs/qemu_mps2_an505_defconfig`
        # in the YasOS root -- to get this gate running again.)
        pytest.skip(str(exc).splitlines()[0])

    assert native_exit == cross_exit, (
        f"Exit code mismatch for {test_file}: "
        f"native={native_exit}, cross={cross_exit}"
    )

    cross_norm = normalize_output(cross_lines)
    native_norm = normalize_output(native_lines, from_fat_runner=True)
    assert native_norm == cross_norm, (
        f"Output mismatch for {test_file}:\n"
        f"--- cross ({len(cross_norm)} lines) ---\n"
        + "\n".join(cross_norm)
        + "\n--- native ({len(native_norm)} lines) ---\n"
        + "\n".join(native_norm)
    )
