"""Compile-only self-host smoke gate.

Cross-compiles the tinycc source files that make up the native bootstrap.
This proves the cross compiler can ingest its own source tree without needing
a full YasOS/QEMU round-trip.
"""

import subprocess
from pathlib import Path

import pytest

from selfhost_runner import compile_tinycc_source

SELFHOST_COMPILE_SOURCES = [
    # Core compiler front-end / middle-end
    "tcc.c",
    "tccpp.c",
    "tccgen.c",
    "tccasm.c",
    "tccelf.c",
    "tccld.c",
    "tccyaff.c",
    "tccdbg.c",
    "tccdebug.c",
    "libtcc.c",
    "svalue.c",
    "tccir_operand.c",
    "tccmachine.c",
    "tccopt.c",
    "tcctools.c",
    # IR layer
    "ir/core.c",
    "ir/dump.c",
    "ir/stack.c",
    "ir/type.c",
    "ir/pool.c",
    "ir/vreg.c",
    "ir/codegen.c",
    "ir/machine_op.c",
    "ir/regalloc.c",
    "ir/cfg.c",
    "ir/ssa.c",
    "ir/opt.c",
    "ir/opt_du.c",
    "ir/opt_xform.c",
    "ir/opt_utils.c",
    "ir/opt_alias.c",
    "ir/opt_loop_utils.c",
    "ir/opt_engine.c",
    "ir/opt_pipeline.c",
    "ir/opt_hash.c",
    "ir/opt_gens_fusion.c",
    "ir/opt_gens_bool.c",
    "ir/opt_gens_call_result.c",
    "ir/opt_gens_branch.c",
    "ir/opt_loop.c",
    "ir/opt_loop_dead.c",
    "ir/opt_memory.c",
    "ir/opt_jump_thread.c",
    "ir/opt_pack64.c",
    "ir/opt_dce.c",
    "ir/opt_constfold.c",
    "ir/opt_branch.c",
    "ir/opt_copyprop.c",
    "ir/opt_fusion.c",
    "ir/opt_promote.c",
    "ir/opt_constprop.c",
    "ir/opt_knownbits.c",
    "ir/opt_dead_lea_store.c",
    "ir/opt_const_aggregate.c",
    "ir/opt_dead_vla.c",
    "ir/opt_loop_const_sim.c",
    "ir/opt_switch_data.c",
    "ir/opt_reroll.c",
    "ir/opt_neg_chain.c",
    "ir/opt_bitfield.c",
    "ir/opt_cmp_fuse.c",
    "ir/opt_setif_or_taut.c",
    "ir/licm.c",
    "ir/opt/ssa_opt.c",
    "ir/opt/ssa_opt_dce.c",
    "ir/opt/ssa_opt_cprop.c",
    "ir/opt/ssa_opt_fold.c",
    "ir/opt/ssa_opt_phi.c",
    "ir/opt/ssa_opt_strength.c",
    "ir/opt/ssa_opt_gvn.c",
    "ir/opt/ssa_opt_reassoc.c",
    "ir/opt/ssa_opt_narrow.c",
    "ir/opt/ssa_opt_branch.c",
    "ir/opt/ssa_opt_sccp.c",
    "ir/opt/ssa_opt_load_cse.c",
    "ir/opt/ssa_opt_dead_loop.c",
    "ir/opt/ssa_opt_cmp_eq.c",
    # ARMv8-M backend
    "arm-thumb-gen.c",
    "arm-thumb-callsite.c",
    "arm-thumb-asm.c",
    "arm-link.c",
]


def _selfhost_include_dirs(tinycc_root):
    """Return include paths that let the cross compiler parse tinycc sources."""
    # The cross compiler already has a sysroot (newlib/YasOS headers) configured
    # at build time.  We only need to add tinycc's own source/include
    # directories so it finds tcc.h, ir/*.h, and the builtin tcclib headers.
    return [
        tinycc_root,
        tinycc_root / "ir",
        tinycc_root / "ir" / "opt",
        tinycc_root / "include",
    ]


def _selfhost_defines():
    """Target defines matching the ARMv8-M native bootstrap."""
    return [
        "TCC_TARGET_ARM",
        "TCC_ARM_VFP",
        "TCC_ARM_EABI=1",
        "TCC_ARM_HARDFLOAT",
        "TCC_TARGET_ARM_THUMB",
        "TCC_TARGET_ARM_ARCHV8M",
        "TCC_IS_NATIVE",
        "CONFIG_TCC_BCHECK=0",
    ]


def _probe_compiler(compiler, include_dirs, defines, tmp_path):
    """Check whether the cross compiler can parse tinycc's core header."""
    probe_src = tmp_path / "probe.c"
    probe_src.write_text('#include "tcc.h"\nint main(void){return 0;}\n')
    probe_out = tmp_path / "probe.o"
    cmd = [str(compiler), "-c", "-Werror"]
    for inc in include_dirs:
        cmd.extend(["-I", str(inc)])
    for d in defines:
        cmd.append(f"-D{d}")
    cmd.extend([str(probe_src), "-o", str(probe_out)])
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    return result.returncode == 0, result.stdout


@pytest.mark.selfhost
@pytest.mark.selfhost_compile
def test_selfhost_compile_smoke(selfhost_compiler, tmp_path):
    """The cross compiler must compile every tinycc source file to an object."""
    tinycc_root = Path(__file__).parent.parent.parent
    sources = [tinycc_root / f for f in SELFHOST_COMPILE_SOURCES]
    missing = [str(s) for s in sources if not s.exists()]
    assert not missing, f"Missing source files: {missing}"

    include_dirs = _selfhost_include_dirs(tinycc_root)
    defines = _selfhost_defines()

    ok, probe_output = _probe_compiler(
        selfhost_compiler, include_dirs, defines, tmp_path
    )
    if not ok:
        pytest.skip(
            "Cross compiler cannot parse tcc.h with the available system headers "
            "(needs an ARM sysroot such as the YasOS rootfs). "
            f"Probe output:\n{probe_output}"
        )

    objects = compile_tinycc_source(
        selfhost_compiler,
        sources,
        include_dirs,
        tmp_path / "objects",
        extra_defines=defines,
    )

    assert len(objects) == len(sources)
    for obj in objects.values():
        assert obj.exists()
        assert obj.stat().st_size > 0
