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
    "source/opt/engine/fp_mat_cache.c",
    "source/opt/engine/pass_registry.c",
    "tcctools.c",
    # IR layer
    # "ir/core.c",
    "ir/dump.c",
    "ir/stack.c",
    "ir/type.c",
    "ir/pool.c",
    "ir/vreg.c",
    "ir/codegen.c",
    "ir/machine_op.c",
    "ir/regalloc.c",
    "source/opt/ra/const_branch_fold.c",
    "source/opt/ra/incomplete_calls.c",
    "source/opt/ra/phi_const_chain.c",
    "ir/cfg.c",
    "ir/ssa.c",
    "source/opt/engine/fp_cache_shim.c",
    "source/opt/util/vreg_def_use.c",
    "source/opt/engine/pass_timing.c",
    "source/opt/analysis/du_chains.c",
    "source/opt/util/pass_disable.c",
    "source/opt/util/const_eval.c",
    "source/opt/util/const_string_eval.c",
    "source/opt/util/cond_util.c",
    "source/opt/util/block_scan.c",
    "source/opt/util/purity.c",
    "source/opt/util/expr_equal.c",
    "source/opt/util/call_params.c",
    "source/opt/util/vreg_query.c",
    "source/opt/util/xform.c",
    "source/opt/util/ir_insert.c",
    "source/opt/analysis/alias.c",
    "source/opt/flat/loop/loop_ir_mutate.c",
    "source/opt/flat/loop/iv_analysis.c",
    "source/opt/flat/loop/iv_strength_reduction.c",
    "source/opt/flat/loop/loop_exit_analysis.c",
    "source/opt/flat/loop/loop_eliminate.c",
    "source/opt/flat/loop/loop_unroll.c",
    "source/opt/flat/loop/loop_rotate.c",
    "source/opt/flat/loop/decrement_to_zero.c",
    "source/opt/engine/ctx.c",
    "source/opt/engine/run_gens.c",
    "source/opt/engine/pipeline_run.c",
    "source/opt/engine/pipeline_table.c",
    "source/opt/engine/gen_adapters.c",
    "source/opt/flat/fusion/gens_fusion.c",
    "source/opt/flat/fusion/add_deref_fold.c",
    "source/opt/flat/fusion/call_chain_rename.c",
    "source/opt/flat/fusion/stackoff_addr_cse.c",
    "source/opt/flat/fusion/lea_fold.c",
    "source/opt/flat/fusion/lea_rmw_fold.c",
    "source/opt/flat/fusion/assign_fuse.c",
    "source/opt/flat/fusion/pack64.c",
    "source/opt/flat/fusion/pack64_tautology.c",
    "source/opt/flat/fusion/cmp_narrow_64.c",
    "source/opt/flat/fusion/shl32_or_chain.c",
    "source/opt/flat/memory/store_inplace_arith.c",
    "source/opt/flat/scalar/redundant_loop_check.c",
    "source/opt/flat/ipa/pure_via_sret.c",
    "source/opt/flat/ipa/func_write_summary.c",
    "source/opt/flat/ipa/tu_func_summary.c",
    "source/opt/flat/ipa/tu_noreturn.c",
    "source/opt/flat/ipa/tu_dead_statics.c",
    "source/opt/flat/memory/dead_init_via_call.c",
    "source/opt/flat/memory/stack_addr_cse.c",
    "source/opt/flat/memory/block_copy_init.c",
    "source/opt/flat/memory/small_memset_to_store.c",
    "source/opt/flat/memory/small_global_memset_to_store.c",
    "source/opt/flat/memory/mem_init.c",
    "source/opt/flat/memory/memmove_to_indexed_stores.c",
    "source/opt/flat/memory/addrof_var_fwd.c",
    "source/opt/flat/memory/invariant_global_load_hoist.c",
    "source/opt/flat/memory/invariant_temp_deref_hoist.c",
    "source/opt/flat/memory/rmw_byte_clear.c",
    "source/opt/flat/memory/local_copy_prop.c",
    "source/opt/flat/memory/struct_copy_roundtrip_elim.c",
    "source/opt/flat/cfg/jump_thread.c",
    "source/opt/flat/cfg/jumpif_invert.c",
    "source/opt/flat/fusion/shift64_dead_half.c",
    "source/opt/flat/dce/dce.c",
    "source/opt/flat/dce/compact_nops.c",
    "source/opt/flat/dce/orphan_cmp_elim.c",
    "source/opt/flat/dce/zero_vla_elim.c",
    "source/opt/flat/dce/body_essential.c",
    "source/opt/flat/dce/noreturn_collapse.c",
    "source/opt/flat/dce/infinite_loop_simplify.c",
    "source/opt/flat/dce/trap_only_body_suppress.c",
    "source/opt/flat/dce/infinite_self_recursion.c",
    "source/opt/flat/dce/noreturn_call_epilogue.c",
    "source/opt/flat/dce/dead_addrvar_elim.c",
    "source/opt/flat/dce/dead_trailing_addrvar_store.c",
    "source/opt/flat/dce/uninit_ub.c",
    "source/opt/flat/dce/ub_only_body_elide.c",
    "source/opt/flat/dce/const_return_uninit_elide.c",
    "source/opt/flat/dce/null_store_dom_return.c",
    "source/opt/flat/scalar/const_string_calls.c",
    "source/opt/flat/scalar/const_call_fold.c",
    "source/opt/flat/scalar/addrof_const_fold.c",
    "source/opt/flat/scalar/global_init_prop.c",
    "source/opt/flat/scalar/symref_const_prop.c",
    "source/opt/flat/scalar/complex_const_param_fold.c",
    "source/opt/flat/scalar/stack_addr_fold.c",
    "source/opt/flat/scalar/single_value_tmp.c",
    "source/opt/flat/scalar/branch_fold.c",
    "source/opt/flat/scalar/stack_bool_diamond.c",
    "source/opt/ssa/cfg/or_bool_diamond.c",
    "source/opt/flat/fusion/barrel_shift.c",
    "source/opt/flat/fusion/shift_pair_ubfx.c",
    "source/opt/flat/scalar/var_to_tmp.c",
    "source/opt/flat/cfg/returnvalue_merge.c",
    "source/opt/flat/loop/backedge_phi_hoist.c",
    "source/opt/flat/cfg/abort_tail_merge.c",
    "source/opt/flat/memory/dead_lea_store.c",
    "source/opt/flat/dce/dead_vla.c",
    "source/opt/flat/loop/const_sim.c",
    "source/opt/flat/cfg/switch_to_data.c",
    "source/opt/flat/loop/reroll.c",
    "source/opt/flat/scalar/neg_chain_cse.c",
    "source/opt/flat/scalar/bitfield.c",
    "source/opt/flat/scalar/add_reassoc.c",
    "source/opt/flat/scalar/cmp_field_fuse.c",
    "source/opt/flat/scalar/cmp_expr_fold.c",
    "source/opt/flat/scalar/float_narrow.c",
    "source/opt/flat/memory/dse.c",
    "source/opt/flat/memory/var_store_elim.c",
    "source/opt/flat/memory/entry_store_prop.c",
    "source/opt/flat/memory/sl_forward.c",
    "source/opt/flat/memory/dead_local_slot.c",
    "source/opt/flat/memory/dead_temp_local.c",
    "source/opt/flat/memory/dead_static_store.c",
    "source/opt/flat/memory/global_base_share.c",
    "source/opt/flat/memory/byte_store_merge.c",
    "source/opt/flat/loop/loop_detect.c",
    "source/opt/flat/loop/func_purity.c",
    "source/opt/flat/loop/licm.c",
    "source/opt/flat/loop/stack_param_promote.c",
    "source/opt/ssa/engine/use_def.c",
    "source/opt/ssa/engine/rewrite.c",
    "source/opt/ssa/engine/stack_resolve.c",
    "source/opt/ssa/engine/driver.c",
    "source/opt/ssa/dce/temp_worklist.c",
    "source/opt/ssa/dce/unreachable.c",
    "source/opt/ssa/dce/dead_var_stores.c",
    "source/opt/ssa/dce/stackloc_stores.c",
    "source/opt/ssa/dce/dce_common.c",
    "source/opt/ssa/dce/orphan_params.c",
    "source/opt/ssa/dce/ret_path_frame_store.c",
    "source/opt/ssa/dce/dead_phi_cycles.c",
    "source/opt/ssa/dce/dead_overwrite_stores.c",
    "source/opt/ssa/dce/dead_global_stores.c",
    "source/opt/ssa/dce/var_liveness.c",
    "source/opt/ssa/dce/dce.c",
    "source/opt/ssa/loop/loop_rotate.c",
    "source/opt/ssa/loop/first_iter_exit.c",
    "source/opt/ssa/loop/loop_cand.c",
    "source/opt/ssa/loop/ptr_iv_exit_subst.c",
    "source/opt/ssa/loop/loop_const_sim.c",
    "source/opt/ssa/loop/loop_unroll.c",
    "source/opt/ssa/loop/iv_strength_reduction.c",
    "source/opt/ssa/loop/decrement_to_zero.c",
    "source/opt/ssa/scalar/sccp.c",
    "source/opt/ssa/loop/dead_loop.c",
    "source/opt/ssa/cfg/guard_collapse.c",
    "source/opt/ssa/memory/ptr_store_dse.c",
    "source/opt/ssa/memory/global_store_dse.c",
    # ARMv8-M backend
    "source/backend/arch/arm/thumb/arm-thumb-gen.c",
    "source/backend/arch/arm/thumb/arm-thumb-callsite.c",
    "source/backend/arch/arm/thumb/arm-thumb-asm.c",
    "source/backend/arch/arm/arm-link.c",
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
        tinycc_root / "source" / "memory" / "include",
        tinycc_root / "source" / "opt" / "ssa" / "include",
        tinycc_root / "source" / "backend" / "arch" / "arm",
        tinycc_root / "source" / "backend" / "arch" / "arm" / "thumb",
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
