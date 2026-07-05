#!/bin/bash
# Bisect which TCC_DISABLE_PASS fixes each failing seed.
# Usage: ./bisect_pass.sh <seed.c> <bad_olevel> <good_checksum>
SEED="$1"
OLEVEL="$2"
GOOD="$3"

cd tests/ir_tests || exit 1

run() {
  TCC_DISABLE_PASS="$1" python run.py -c "../../$SEED" --cflags="$OLEVEL" 2>/dev/null | grep -o 'checksum=[0-9a-f]*'
}

PASSES="uninit_ub uninit_dom_ret dce const_prop const_var_prop global_init symref_prop global_sl_fwd const_prop_tmp const_agg_fold known_bits neg_chain_cse add_reassoc redundant_assign string_calls self_copy_elim value_tracking cmp_expr_fold self_arith cmp_offset_fold branch_fold switch_collapse stack_nonnull setif_fuse stack_bool or_bool setif_or_taut var_tmp_fwd var_to_tmp nonneg_fold float_branch vrp single_val_tmp float_narrow deref_fwd fusion_mla deref_indexed disp_fusion copy_prop chain_fold pair_reorder postinc bool_simplify sl_forward bf_insert_extract cmp_field_fuse const_cascade branch_fold_2x jump_thread elim_fallthru kb_cascade branch_cleanup dead_vla_struct alloca_load_fwd zero_vla byte_store_merge store_redundant dse dead_static_store dead_var_store dead_addrvar dead_trail_addrvar dead_alloca_vreg dead_local_slot dead_lea_store dead_temp_local inplace_arith global_base_share orphan_cmp inf_loop_simpl dead_pre_inf return_reuse entry_store esp_cleanup"

echo "=== baseline ($OLEVEL, no disable) ==="
NONE=$(run "")
echo "$NONE (good=$GOOD)"

echo "=== bisecting passes ==="
for p in $PASSES; do
  R=$(run "$p")
  if [ "$R" != "$NONE" ]; then
    MATCH=""
    [ "$R" = "checksum=$GOOD" ] && MATCH=" *** FIXES (matches good) ***"
    echo "$p -> $R$MATCH"
  fi
done
