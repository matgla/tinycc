# SValue Pool Deduplication & Field Packing

Reduce memory footprint by deduplicating identical SValues in the pool and packing SValue fields to shrink each entry from ~48 bytes to ~40 bytes.

---

## Part 1: SValue Deduplication in Pool

**Goal:** Avoid storing duplicate SValues (e.g., repeated constant `0`, same vreg references) by returning existing pool index when an identical entry exists.

### Steps

1. **Add hash function** in `tccir.c` — implement `svalue_hash(const SValue *sv)` that hashes key fields: `r`, `vr`, `type.t`, `c.i`, and `sym` pointer. Use FNV-1a or simple multiply-xor.

2. **Add equality function** — implement `svalue_equal(const SValue *a, const SValue *b)` comparing all semantically relevant fields (skip `pr0`/`pr1` which are codegen temporaries).

3. **Add hash table to TCCIRState** in `tccir.h` (near line 321) — add `int *svalue_hash_buckets` and `int svalue_hash_size` fields for a simple open-addressed hash table mapping hash → pool index.

4. **Modify `tcc_ir_svalue_pool_add()`** in `tccir.c` (line 718) — before appending, compute hash and probe table; if match found via `svalue_equal()`, return existing index; otherwise insert and update hash table.

5. **Update pool free/init** — initialize and free the hash table in `tcc_ir_svalue_pool_init()` and `tcc_ir_svalue_pool_free()`.

---

## Part 2: SValue Field Packing

**Goal:** Reduce SValue size from ~48 bytes to ~40 bytes by packing small fields and reviewing alignment.

### Steps

1. **Pack register fields** in `tcc.h` (line 408) — combine `pr0` (u8) + `pr1` (u8) + `r` (u16) into a single `uint32_t regs` or use bitfields; eliminates 2-4 bytes padding before `CType`.

2. **Shrink `vr` if feasible** — if vreg count is always < 32K, use `int16_t vr` instead of `int vr` (saves 2 bytes).

3. **Review CValue alignment** — `CValue` union is 16 bytes due to `long double`; if ARMv8-M target doesn't use x87 `long double`, consider `#ifdef` to reduce to 8-byte `double` max (saves 8 bytes per SValue).

4. **Add `__attribute__((packed))` selectively** — if natural packing doesn't achieve target, use packed attribute on SValue (with performance testing on hot paths).

5. **Verify with `sizeof` assertions** — add `_Static_assert(sizeof(SValue) <= 40, "...")` to catch regressions.

---

## Further Considerations

1. **Hash table sizing** — start with load factor ~0.7; resize when pool grows. Use pool index + 1 as stored value (0 = empty bucket). Linear probing is simplest.

2. **Dedup scope** — only deduplicate immutable operands (constants, vregs); operands modified during codegen (`pr0`/`pr1` assignment) should be excluded. Recommend: only dedup constants (`VT_CONST` with `vr == -1`).

3. **Backward compatibility** — `tcc_ir_writeback_quad()` mutates pool entries; if deduplication shares entries, writes would corrupt other instructions. Options:
   - (A) Copy-on-write
   - (B) Only dedup read-only constants
   - (C) Separate mutable/immutable pools
   
   **Recommend option B** for simplicity.