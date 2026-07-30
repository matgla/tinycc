"""Compile-only self-host smoke gate.

Cross-compiles the tinycc source files that make up the native bootstrap.
This proves the cross compiler can ingest its own source tree without needing
a full YasOS/QEMU round-trip.

The source list mirrors what `build_rootfs.sh` compiles with armv8m-tcc in
the native stage: TCC_FILES from the top-level Makefile plus the ARM backend
archive sources (libarm.a / libthumb.a / FPU helpers).
`test_selfhost_source_list_in_sync` guards the list against drifting from the
Makefiles — a stale list is how the plain-`arm` predefine regression in
source/opt/ssa/scalar/fold.c escaped this gate.
"""

import re
import subprocess
from pathlib import Path

import pytest

from selfhost_runner import compile_tinycc_source

SELFHOST_COMPILE_SOURCES = [
    # top-level
    "libtcc.c",
    "svalue.c",
    "tcc.c",
    "tccasm.c",
    "tccdbg.c",
    "tccdebug.c",
    "tccelf.c",
    "tccgen.c",
    "tccir_operand.c",
    "tccld.c",
    "tccls.c",
    "tccmachine.c",
    "tccpp.c",
    "tcctools.c",
    "tccyaff.c",
    # ir/
    "ir/cfg.c",
    "ir/codegen.c",
    "ir/dump.c",
    "ir/machine_op.c",
    "ir/pool.c",
    "ir/regalloc.c",
    "ir/ssa.c",
    "ir/stack.c",
    "ir/type.c",
    "ir/vreg.c",
    # ir/gen/
    "ir/gen/arith.c",
    "ir/gen/asm.c",
    "ir/gen/config.c",
    "ir/gen/control.c",
    "ir/gen/float.c",
    "ir/gen/jump.c",
    "ir/gen/live.c",
    "ir/gen/params.c",
    "ir/gen/put.c",
    "ir/gen/softfloat.c",
    "ir/gen/state.c",
    "ir/gen/util.c",
    "ir/gen/vla.c",
    # source/backend/arch/arm/
    "source/backend/arch/arm/arm-link.c",
    "source/backend/arch/arm/arm.c",
    "source/backend/arch/arm/arm_aapcs.c",
    "source/backend/arch/arm/arm_regalloc.c",
    "source/backend/arch/arm/ssa_opt_arm.c",
    # source/backend/arch/arm/thumb/
    "source/backend/arch/arm/thumb/arm-thumb-asm.c",
    "source/backend/arch/arm/thumb/arm-thumb-callsite.c",
    "source/backend/arch/arm/thumb/arm-thumb-gen.c",
    "source/backend/arch/arm/thumb/thop_adr.c",
    "source/backend/arch/arm/thumb/thop_alu_imm.c",
    "source/backend/arch/arm/thumb/thop_alu_reg.c",
    "source/backend/arch/arm/thumb/thop_bitfield.c",
    "source/backend/arch/arm/thumb/thop_block.c",
    "source/backend/arch/arm/thumb/thop_branch.c",
    "source/backend/arch/arm/thumb/thop_cmp.c",
    "source/backend/arch/arm/thumb/thop_coproc.c",
    "source/backend/arch/arm/thumb/thop_dsp.c",
    "source/backend/arch/arm/thumb/thop_extend.c",
    "source/backend/arch/arm/thumb/thop_ldaex.c",
    "source/backend/arch/arm/thumb/thop_ldr_literal.c",
    "source/backend/arch/arm/thumb/thop_ldrd.c",
    "source/backend/arch/arm/thumb/thop_ldrex.c",
    "source/backend/arch/arm/thumb/thop_mem_exclusive.c",
    "source/backend/arch/arm/thumb/thop_mem_imm.c",
    "source/backend/arch/arm/thumb/thop_mem_reg.c",
    "source/backend/arch/arm/thumb/thop_mem_unpriv.c",
    "source/backend/arch/arm/thumb/thop_mov.c",
    "source/backend/arch/arm/thumb/thop_mrs.c",
    "source/backend/arch/arm/thumb/thop_mul.c",
    "source/backend/arch/arm/thumb/thop_mvn.c",
    "source/backend/arch/arm/thumb/thop_pld.c",
    "source/backend/arch/arm/thumb/thop_rev.c",
    "source/backend/arch/arm/thumb/thop_shift_imm.c",
    "source/backend/arch/arm/thumb/thop_shift_reg.c",
    "source/backend/arch/arm/thumb/thop_system.c",
    "source/backend/arch/arm/thumb/thop_tbb.c",
    "source/backend/arch/arm/thumb/thop_vfp.c",
    "source/backend/arch/arm/thumb/thumb.c",
    # source/backend/arch/fpu/arm/
    "source/backend/arch/fpu/arm/fpv5-d16.c",
    "source/backend/arch/fpu/arm/fpv5-sp-d16.c",
    "source/backend/arch/fpu/arm/rp2350-dcp.c",
    # source/backend/generators/
    "source/backend/generators/function.c",
    "source/backend/generators/regalloc.c",
    # source/memory/
    "source/memory/unique_ptr.c",
    "source/memory/vector.c",
    # source/opt/
    "source/opt/function_pipeline.c",
    # source/opt/analysis/
    "source/opt/analysis/alias.c",
    "source/opt/analysis/du_chains.c",
    "source/opt/analysis/mem_ssa.c",
    "source/opt/analysis/memref.c",
    # source/opt/engine/
    "source/opt/engine/ctx.c",
    "source/opt/engine/fp_cache_shim.c",
    "source/opt/engine/fp_mat_cache.c",
    "source/opt/engine/gen_adapters.c",
    "source/opt/engine/pass_registry.c",
    "source/opt/engine/pass_timing.c",
    "source/opt/engine/pipeline_run.c",
    "source/opt/engine/pipeline_table.c",
    "source/opt/engine/run_gens.c",
    # source/opt/flat/cfg/
    "source/opt/flat/cfg/abort_tail_merge.c",
    "source/opt/flat/cfg/if_convert.c",
    "source/opt/flat/cfg/jump_thread.c",
    "source/opt/flat/cfg/jumpif_invert.c",
    "source/opt/flat/cfg/returnvalue_merge.c",
    "source/opt/flat/cfg/switch_to_data.c",
    # source/opt/flat/dce/
    "source/opt/flat/dce/body_essential.c",
    "source/opt/flat/dce/compact_nops.c",
    "source/opt/flat/dce/const_return_uninit_elide.c",
    "source/opt/flat/dce/dce.c",
    "source/opt/flat/dce/dead_addrvar_elim.c",
    "source/opt/flat/dce/dead_trailing_addrvar_store.c",
    "source/opt/flat/dce/dead_vla.c",
    "source/opt/flat/dce/infinite_loop_simplify.c",
    "source/opt/flat/dce/infinite_self_recursion.c",
    "source/opt/flat/dce/noreturn_call_epilogue.c",
    "source/opt/flat/dce/noreturn_collapse.c",
    "source/opt/flat/dce/null_store_dom_return.c",
    "source/opt/flat/dce/orphan_cmp_elim.c",
    "source/opt/flat/dce/trap_only_body_suppress.c",
    "source/opt/flat/dce/ub_only_body_elide.c",
    "source/opt/flat/dce/uninit_ub.c",
    "source/opt/flat/dce/zero_vla_elim.c",
    # source/opt/flat/fusion/
    "source/opt/flat/fusion/add_deref_fold.c",
    "source/opt/flat/fusion/assign_fuse.c",
    "source/opt/flat/fusion/barrel_shift.c",
    "source/opt/flat/fusion/call_chain_rename.c",
    "source/opt/flat/fusion/cmp_narrow_64.c",
    "source/opt/flat/fusion/disp.c",
    "source/opt/flat/fusion/gens_fusion.c",
    "source/opt/flat/fusion/indexed_chain.c",
    "source/opt/flat/fusion/lea_fold.c",
    "source/opt/flat/fusion/lea_rmw_fold.c",
    "source/opt/flat/fusion/pack64.c",
    "source/opt/flat/fusion/pack64_tautology.c",
    "source/opt/flat/fusion/pair_reorder.c",
    "source/opt/flat/fusion/shift64_dead_half.c",
    "source/opt/flat/fusion/shift_pair_ubfx.c",
    "source/opt/flat/fusion/shl32_or_chain.c",
    "source/opt/flat/fusion/stackoff_addr_cse.c",
    # source/opt/flat/ipa/
    "source/opt/flat/ipa/func_write_summary.c",
    "source/opt/flat/ipa/pure_via_sret.c",
    "source/opt/flat/ipa/tu_dead_statics.c",
    "source/opt/flat/ipa/tu_func_summary.c",
    "source/opt/flat/ipa/tu_noreturn.c",
    # source/opt/flat/loop/
    "source/opt/flat/loop/backedge_phi_hoist.c",
    "source/opt/flat/loop/const_sim.c",
    "source/opt/flat/loop/decrement_to_zero.c",
    "source/opt/flat/loop/func_purity.c",
    "source/opt/flat/loop/iv_analysis.c",
    "source/opt/flat/loop/iv_strength_reduction.c",
    "source/opt/flat/loop/licm.c",
    "source/opt/flat/loop/loop_detect.c",
    "source/opt/flat/loop/loop_eliminate.c",
    "source/opt/flat/loop/loop_exit_analysis.c",
    "source/opt/flat/loop/loop_ir_mutate.c",
    "source/opt/flat/loop/loop_relayout.c",
    "source/opt/flat/loop/loop_rotate.c",
    "source/opt/flat/loop/loop_unroll.c",
    "source/opt/flat/loop/reroll.c",
    "source/opt/flat/loop/seq_guard_elim.c",
    "source/opt/flat/loop/stack_param_promote.c",
    # source/opt/flat/memory/
    "source/opt/flat/memory/addrof_var_fwd.c",
    "source/opt/flat/memory/bitfield_unit_narrow.c",
    "source/opt/flat/memory/block_copy_init.c",
    "source/opt/flat/memory/byte_store_merge.c",
    "source/opt/flat/memory/copy_source_load_fwd.c",
    "source/opt/flat/memory/dead_init_via_call.c",
    "source/opt/flat/memory/dead_lea_store.c",
    "source/opt/flat/memory/dead_local_slot.c",
    "source/opt/flat/memory/dead_static_store.c",
    "source/opt/flat/memory/dead_temp_local.c",
    "source/opt/flat/memory/dse.c",
    "source/opt/flat/memory/entry_store_prop.c",
    "source/opt/flat/memory/global_base_share.c",
    "source/opt/flat/memory/global_deref_cse.c",
    "source/opt/flat/memory/invariant_global_load_hoist.c",
    "source/opt/flat/memory/invariant_temp_deref_hoist.c",
    "source/opt/flat/memory/local_copy_prop.c",
    "source/opt/flat/memory/mem_init.c",
    "source/opt/flat/memory/mem_inline.c",
    "source/opt/flat/memory/memmove_to_indexed_stores.c",
    "source/opt/flat/memory/ptr_local_fwd.c",
    "source/opt/flat/memory/rmw_byte_clear.c",
    "source/opt/flat/memory/sl_forward.c",
    "source/opt/flat/memory/small_global_memset_to_store.c",
    "source/opt/flat/memory/small_memset_to_store.c",
    "source/opt/flat/memory/stack_addr_cse.c",
    "source/opt/flat/memory/store_inplace_arith.c",
    "source/opt/flat/memory/struct_copy_roundtrip_elim.c",
    "source/opt/flat/memory/symaddr_cse.c",
    "source/opt/flat/memory/var_store_elim.c",
    # source/opt/flat/scalar/
    "source/opt/flat/scalar/add_reassoc.c",
    "source/opt/flat/scalar/addrof_const_fold.c",
    "source/opt/flat/scalar/bitfield.c",
    "source/opt/flat/scalar/bool.c",
    "source/opt/flat/scalar/branch.c",
    "source/opt/flat/scalar/branch_fold.c",
    "source/opt/flat/scalar/call_result.c",
    "source/opt/flat/scalar/cmp_expr_fold.c",
    "source/opt/flat/scalar/cmp_field_fuse.c",
    "source/opt/flat/scalar/complex_const_param_fold.c",
    "source/opt/flat/scalar/const_aggregate.c",
    "source/opt/flat/scalar/const_call_fold.c",
    "source/opt/flat/scalar/const_prop_tmp.c",
    "source/opt/flat/scalar/const_string_calls.c",
    "source/opt/flat/scalar/const_var_prop.c",
    "source/opt/flat/scalar/float_narrow.c",
    "source/opt/flat/scalar/global_init_prop.c",
    "source/opt/flat/scalar/known_bits.c",
    "source/opt/flat/scalar/narrow_store.c",
    "source/opt/flat/scalar/neg_chain_cse.c",
    "source/opt/flat/scalar/redundant_loop_check.c",
    "source/opt/flat/scalar/self_arith.c",
    "source/opt/flat/scalar/self_copy.c",
    "source/opt/flat/scalar/single_value_tmp.c",
    "source/opt/flat/scalar/stack_addr_fold.c",
    "source/opt/flat/scalar/stack_bool_diamond.c",
    "source/opt/flat/scalar/switch_collapse.c",
    "source/opt/flat/scalar/symref_const_prop.c",
    "source/opt/flat/scalar/value_tracking.c",
    "source/opt/flat/scalar/var_tmp_fwd.c",
    "source/opt/flat/scalar/var_to_tmp.c",
    # source/opt/ra/
    "source/opt/ra/const_branch_fold.c",
    "source/opt/ra/incomplete_calls.c",
    "source/opt/ra/phi_const_chain.c",
    # source/opt/ssa/cfg/
    "source/opt/ssa/cfg/branch.c",
    "source/opt/ssa/cfg/cmp_eq.c",
    "source/opt/ssa/cfg/guard_collapse.c",
    "source/opt/ssa/cfg/or_bool_diamond.c",
    "source/opt/ssa/cfg/phi.c",
    "source/opt/ssa/cfg/vrp.c",
    # source/opt/ssa/dce/
    "source/opt/ssa/dce/dce.c",
    "source/opt/ssa/dce/dce_common.c",
    "source/opt/ssa/dce/dead_global_stores.c",
    "source/opt/ssa/dce/dead_overwrite_stores.c",
    "source/opt/ssa/dce/dead_phi_cycles.c",
    "source/opt/ssa/dce/dead_var_stores.c",
    "source/opt/ssa/dce/orphan_params.c",
    "source/opt/ssa/dce/ret_path_frame_store.c",
    "source/opt/ssa/dce/stackloc_stores.c",
    "source/opt/ssa/dce/temp_worklist.c",
    "source/opt/ssa/dce/unreachable.c",
    "source/opt/ssa/dce/var_liveness.c",
    # source/opt/ssa/engine/
    "source/opt/ssa/engine/driver.c",
    "source/opt/ssa/engine/rewrite.c",
    "source/opt/ssa/engine/stack_resolve.c",
    "source/opt/ssa/engine/use_def.c",
    # source/opt/ssa/loop/
    "source/opt/ssa/loop/dead_loop.c",
    "source/opt/ssa/loop/decrement_to_zero.c",
    "source/opt/ssa/loop/first_iter_exit.c",
    "source/opt/ssa/loop/iv_strength_reduction.c",
    "source/opt/ssa/loop/loop_cand.c",
    "source/opt/ssa/loop/loop_const_sim.c",
    "source/opt/ssa/loop/loop_rotate.c",
    "source/opt/ssa/loop/loop_unroll.c",
    "source/opt/ssa/loop/ptr_iv_exit_subst.c",
    # source/opt/ssa/memory/
    "source/opt/ssa/memory/diamond_store_fwd.c",
    "source/opt/ssa/memory/global_addr_hoist.c",
    "source/opt/ssa/memory/global_store_dse.c",
    "source/opt/ssa/memory/load_cse.c",
    "source/opt/ssa/memory/ptr_store_dse.c",
    # source/opt/ssa/scalar/
    "source/opt/ssa/scalar/bitop_const_fold.c",
    "source/opt/ssa/scalar/bool_norm.c",
    "source/opt/ssa/scalar/cmp_offset_fold.c",
    "source/opt/ssa/scalar/cprop.c",
    "source/opt/ssa/scalar/fold.c",
    "source/opt/ssa/scalar/gvn.c",
    "source/opt/ssa/scalar/narrow.c",
    "source/opt/ssa/scalar/reassoc.c",
    "source/opt/ssa/scalar/sccp.c",
    "source/opt/ssa/scalar/setif_or_taut.c",
    "source/opt/ssa/scalar/strength.c",
    "source/opt/ssa/scalar/tmp_block_const.c",
    "source/opt/ssa/scalar/var_imm_prop.c",
    # source/opt/ssa/string/
    "source/opt/ssa/string/const_string_fold.c",
    "source/opt/ssa/string/str_memchr.c",
    "source/opt/ssa/string/str_strchr.c",
    "source/opt/ssa/string/str_strcmp.c",
    "source/opt/ssa/string/str_strcpy.c",
    "source/opt/ssa/string/str_strlen.c",
    "source/opt/ssa/string/str_strspn.c",
    "source/opt/ssa/string/str_strstr.c",
    # source/opt/util/
    "source/opt/util/block_scan.c",
    "source/opt/util/call_params.c",
    "source/opt/util/cond_util.c",
    "source/opt/util/const_eval.c",
    "source/opt/util/const_string_eval.c",
    "source/opt/util/expr_equal.c",
    "source/opt/util/ir_insert.c",
    "source/opt/util/pass_disable.c",
    "source/opt/util/purity.c",
    "source/opt/util/vreg_def_use.c",
    "source/opt/util/vreg_query.c",
    "source/opt/util/xform.c",
]


def _selfhost_include_dirs(tinycc_root):
    """Return include paths that let the cross compiler parse tinycc sources.

    Mirrors the -I set the top-level Makefile passes when building the native
    armv8m-tcc (DEFINES in Makefile + source/opt/*/Makefile SSA/flat blocks).
    The cross compiler already has a sysroot (newlib/YasOS headers) configured
    at build time.
    """
    rel = [
        ".",
        "ir",
        "include",
        "source/opt/include",
        "source/opt/framework",
        "source/opt/ssa",
        "source/opt/ssa/include",
        "source/opt/ssa/string",
        "source/opt/ssa/scalar",
        "source/opt/ssa/memory",
        "source/opt/ssa/cfg",
        "source/opt/ssa/dce",
        "source/opt/ssa/loop",
        "source/opt/flat",
        "source/opt/flat/include",
        "source/opt/flat/scalar",
        "source/opt/flat/cfg",
        "source/opt/flat/fusion",
        "source/opt/flat/memory",
        "source/opt/flat/dce",
        "source/opt/flat/loop",
        "source/opt/flat/ipa",
        "source/memory/include",
        "source/utils/include",
        "source/backend/arch/arm",
        "source/backend/arch/arm/thumb",
    ]
    return [tinycc_root / r for r in rel]


def _selfhost_defines():
    """Target defines matching the ARMv8-M native bootstrap (build_rootfs.sh)."""
    return [
        "TCC_TARGET_ARM",
        "TCC_ARM_VFP",
        "TCC_ARM_EABI=1",
        "TCC_ARM_HARDFLOAT",
        "TCC_TARGET_ARM_THUMB",
        "TCC_TARGET_ARM_ARCHV8M",
        "TCC_IS_NATIVE",
        "TARGETOS_YasOS=1",
        "CONFIG_TCC_BCHECK=0",
        # build_rootfs.sh always defines CONFIG_TCC_DEBUG for the native
        # compiler (on-target -dump-ir support); compile the same code here.
        "TCC_DEBUG=0",
        "CONFIG_TCC_DEBUG",
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
def test_predefined_macros_stay_in_reserved_namespace(selfhost_compiler, tmp_path):
    """Built-in target macros must not claim ordinary identifiers.

    The ARM backend used to predefine plain `arm` / `arm_elf`, which broke
    valid C11 code that uses those words as identifiers: fold.c declared a
    local variable named `arm` and the native bootstrap died with
    "identifier expected".  GCC reserves such unprefixed macros for
    -std=gnu* mode; the bootstrap builds with -std=c11, so every predefined
    macro must start with an underscore.
    """
    empty = tmp_path / "empty.c"
    empty.write_text("\n")
    result = subprocess.run(
        [str(selfhost_compiler), "-E", "-dD", str(empty)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    assert result.returncode == 0, result.stdout
    names = re.findall(r"^#define\s+([A-Za-z_][A-Za-z0-9_]*)", result.stdout, re.M)
    offenders = sorted({n for n in names if not n.startswith("_")})
    assert not offenders, (
        "Predefined macros outside the reserved namespace "
        f"(would break user code that uses them as identifiers): {offenders}"
    )


@pytest.mark.selfhost
@pytest.mark.selfhost_compile
def test_selfhost_source_list_in_sync():
    """SELFHOST_COMPILE_SOURCES must cover every TU the native bootstrap builds.

    Queries the top-level Makefile for TCC_FILES and parses the ARM backend
    sub-Makefile SRCS lists.  Any file compiled by the bootstrap but missing
    here escapes the compile gate entirely.
    """
    tinycc_root = Path(__file__).parent.parent.parent
    if not (tinycc_root / "config.mak").exists():
        pytest.skip("tree not configured (no config.mak); cannot query Makefile")

    result = subprocess.run(
        [
            "make",
            "--no-print-directory",
            '--eval=selfhost-print-files: ; @printf "%s\\n" $(TCC_FILES)',
            "selfhost-print-files",
            "CROSS_TARGET=armv8m",
        ],
        cwd=tinycc_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    if result.returncode != 0:
        pytest.skip(f"make query failed:\n{result.stdout}")

    expected = set()
    for tok in result.stdout.split():
        name = tok[len("armv8m-"):] if tok.startswith("armv8m-") else tok
        if name.endswith(".o"):
            expected.add(name[:-2] + ".c")

    # The backend archive (libarm.a) is built by sub-makes with the same CC;
    # their source lists are plain `SRCS = ...` / `FPU_SRCS = ...` lines.
    arch_dir = "source/backend/arch/arm"
    for makefile, prefix in [
        (f"{arch_dir}/Makefile", arch_dir),
        (f"{arch_dir}/thumb/Makefile", f"{arch_dir}/thumb"),
    ]:
        text = (tinycc_root / makefile).read_text()
        for m in re.finditer(r"^SRCS\s*=\s*(.*)$", text, re.M):
            expected.update(f"{prefix}/{f}" for f in m.group(1).split())
        for m in re.finditer(r"^FPU_SRCS\s*=\s*(.*)$", text, re.M):
            expected.update(
                f"source/backend/arch/fpu/arm/{f}" for f in m.group(1).split()
            )

    missing = sorted(expected - set(SELFHOST_COMPILE_SOURCES))
    assert not missing, (
        "Native-bootstrap TUs missing from SELFHOST_COMPILE_SOURCES "
        f"(the compile gate is not covering them): {missing}"
    )


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
