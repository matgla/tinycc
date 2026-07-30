# orig_index-keyed side tables: bounds

`ir->barrel_shifts`, `ir->shift64_dead_half` and `ir->bfi_params` are side
tables keyed by an instruction's `orig_index`. Each is allocated once, sized
`ir->max_orig_index + 1` at the moment its producing pass runs
(`tcc_ir_barrel_shift_fusion`, `tcc_ir_opt_shift64_dead_half`,
`tcc_ir_opt_bitfield_insert_to_bfi`), all just before register allocation.

The trap: `ir->max_orig_index` keeps growing *after* these tables are sized.
Register allocation and later SSA passes insert phi-resolution copies, spill
loads/stores and similar instructions, each taking a fresh
`orig_index = ++ir->max_orig_index` (see `insert_instruction_at` in
`ir/opt_loop_utils.c`). None of these late instructions can carry an
annotation — the producing pass already ran — but their `orig_index` is past
the end of the table.

So a reader must bound by the table's own allocation length, not by the live
`ir->max_orig_index`. Each table records that length in a paired
`*_len` field, and reads go through the accessors in `tccir.h`
(`tcc_ir_barrel_shift_at`, `tcc_ir_shift64_dead_half_at`,
`tcc_ir_bfi_params_at`), which return 0 (= no annotation) for any
`orig_index` outside `[0, *_len)`.

Reading against `max_orig_index` instead is a heap-buffer-overflow: it was
caught by ASan in the `ssa:dead_loop/simple` golden-IR test, where SCCP
(running inside `tcc_ir_ssa_regalloc`) evaluated a phi copy whose `orig_index`
had grown past the `barrel_shifts` allocation.
