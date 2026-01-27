# Reduce Peak Memory from `tok_str_add2`

## Heaptrack Numbers (hello_world test, armv8m-tcc)

- **Peak heap: 733.88K** (656.06K from default_reallocator)
- `tokstr_alloc` TAL pool chain attributed to tok_str_add2:
  - 131.07K (3rd pool, 128KB) -- triggered by `parse_define -> tok_str_add2`
  - 65.54K (2nd pool, 64KB) -- triggered by `macro_arg_subst -> tok_str_add2`
  - ~32K (initial pool) -- in "other places" bucket
  - **Total: ~228K = 31% of peak heap**
- `toksym_alloc` TAL pools: 49.15K + 98.30K = 147K (separate issue, not addressed here)

## Root Cause

TAL is a linear bump allocator. When `tok_str_realloc` grows a buffer via `tal_realloc`, a new block is allocated and the old one becomes **dead space** -- TAL can only reclaim when ALL pool allocations are freed. Each buffer growth (8 -> 12 -> 18 -> 27 -> ...) wastes the previous allocation. When pools fill with dead space, new pools chain (32KB -> 64KB -> 128KB = 224KB total for just token buffers).

System `realloc` can grow in-place and properly free old blocks. TAL is a poor fit for growing buffers but a good fit for fixed-size struct allocations.

## Proposed Changes

### Change 1: Use system malloc for token **buffers**, keep TAL for TokenString **structs**

**File:** tccpp.c

| Location | Current | New |
|----------|---------|-----|
| `tok_str_free_str` (line 1165) | `tal_free(tokstr_alloc, str)` | `tcc_free(str)` |
| `tok_str_ensure_heap` (line 1184) | `tal_realloc(tokstr_alloc, NULL, ...)` | `tcc_malloc(...)` |
| `tok_str_realloc` inline->heap (line 1207) | `tal_realloc(tokstr_alloc, NULL, ...)` | `tcc_malloc(...)` |
| `tok_str_realloc` grow (line 1221) | `tal_realloc(tokstr_alloc, s->data.str, ...)` | `tcc_realloc(s->data.str, ...)` |
| `TOKSTR_TAL_SIZE` (line 127) | `32 * 1024` | `8 * 1024` (only structs now) |
| `TOKSTR_TAL_LIMIT` (line 129) | `1024` | `128` (structs are ~48-64 bytes) |

Safety: `tok_str_free_str` is called from 10 sites in tccpp.c, 1 in tccgen.c. All pass buffer pointers that will now be from `tcc_malloc`. `tok_str_free` (line 1168) still uses `tal_free(tokstr_alloc, str)` for the struct itself.

**Expected impact:** Eliminates the 64KB + 128KB pool chain allocations (~190K savings). The actual live buffer data is much less than the pools that contained it.

### Change 2: Increase inline buffer from 4 to 8 ints

**File:** tcc.h line 644

Change `TOKSTR_SMALL_BUFSIZE` from 4 to 8. Many macros produce 2-6 ints and will stay fully inline. `sizeof(TokenString)` grows by ~16 bytes; stack impact is negligible (5 stack-allocated instances).

### Change 3: More aggressive `tok_str_shrink`

**File:** tccpp.c lines 1230-1241

With system malloc, shrinking returns memory properly. Remove the `TOKSTR_TAL_LIMIT` guard and shrink when `allocated_len > len + 4`.

## Verification

1. `make clean && make -j8` -- compiler builds
2. Self-compile: `./armv8m-tcc -c tccpp.c`
3. Profile: `tests/ir_tests/profile_suite.py --two-phase` -- compare peak with baseline
