# Bug Reports

Issues observed in the TCC IR optimizer during the bitfld-1 gap-closure work
(2026-05). Each report stands alone; cross-references use `[[NN]]` style.

| #  | Title                                                         | Status     |
|----|---------------------------------------------------------------|------------|
| 01 | `const_prop_tmp` does not fold `IMOD`/`UMOD`/`DIV`/`UDIV`/`PDIV` with two-immediate operands | FIXED      |
| 02 | `SHL N → SHR M` peephole only handles `N == M`; misses bitfield-extract (`N != M`) | FIXED      |
| 03 | `dead_local_slot_elim` ignores STOREs through a LEA temp (`T = Addr[StackLoc[X]]; STORE T***DEREF***`) | FIXED      |
| 04 | `memory_passes` group stalls when its trigger (`sl_forward`) returns 0 mid-cascade | WORKED AROUND |
| 05 | VAR/PARAM operands carry `tag=STACKOFF` for their potential spill slot; conflated with direct stack refs in new passes | FIXED      |
| 06 | `collect_tu_func_summary` missed STORE_INDEXED / STORE_POSTINC writes when `is_lval` was cleared | FIXED      |
| 07 | `dead_static_store_elim` only matched post-fusion SYMREF dest; missed the pre-fusion TEMP-DEREF form | FIXED      |
| 08 | `gen_late_reopt_functions` only iterated `inline_fns`, locking out functions failing `auto_inline_sig_ok` | FIXED      |
| 09 | `lib/builtin.c` word-at-a-time string helpers fail on 64-bit hosts | FIXED      |

Statuses:
- **FIXED**: a code change in this commit/branch resolves it.
- **WORKED AROUND**: the underlying limitation is still present; mitigated by an additional pass or extra pipeline pass.
- **DOCUMENTED**: footgun that bit a new pass author; recorded for next person.
