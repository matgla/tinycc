# tcc -O2 (self-host) vs arm-none-eabi-gcc -O2 — codegen comparison

**Date:** 2026-06-23 · **Target:** Cortex-M33 / armv8m thumb · **Question:** where is the
device tcc leaving compile-time performance on the table, measured against a "good codegen"
reference?

## Method

The device compiler `bin/armv8m-tcc.elf` is built **by tcc compiling its own sources** with
`-O2 -mcpu=cortex-m33` (the self-host stage in `build_rootfs.sh`). To get a reference for how
good that codegen *could* be, I compiled the **same 81 translation units** (CORE + IR + arm
backend, from the Makefile's `armv8m_FILES`) with `arm-none-eabi-gcc -O2 -mcpu=cortex-m33
-mthumb -fpie`, same TCC defines. The gcc build is **not linked or run** — it only exists to
diff codegen quality per function. All 81 TUs compiled (2 needed `-fpermissive` / a `dlfcn.h`
stub; neither is a hotspot).

I then matched functions **by name across both builds** (the `.elf` carries ~3900 symbols incl.
libc/native code the gcc objects don't; comparing only the 1547 functions present in **both**
keeps it apples-to-apples) and weighted everything by `scripts/tcc_profile.py` — the
device-representative CPU profile (callgrind `Ir` on the x86 cross, which runs the identical
codegen path) for the default `-O0` compile of `129_scopes.c`.

**Caveats (read before acting):**
- Code size is a *proxy* for cycles. On the M33 (no data cache) instruction-fetch ∝ size is a
  fair proxy, but data traffic also costs — so the profiler `Ir` weighting, not raw size, is the
  authority on "what's hot."
- gcc and tcc inline differently, which **confounds per-function size** (see §3). I call this out
  where it matters rather than letting it mislead.
- The gcc build drops `TCC_IS_NATIVE` and forces `CONFIG_TCC_STATIC` / `CONFIG_TCC_SEMLOCK=0` to
  build under newlib. These only touch `tcc_run`/threading glue — none of the hot codegen.

## 1. Headline numbers

| metric | value |
|---|---|
| `.text` of device `armv8m-tcc.elf` | **2.26 MB** |
| matched-function total, **gcc -O2** | 1,152,516 B |
| matched-function total, **tcc -O2** | 1,368,164 B |
| **tcc / gcc ratio** | **1.19×** (tcc emits +19% more code on equal functions) |
| `.text` that is **duplicated inline-helper copies** | **~226 KB (10% of .text)** |

Two distinct, independently-actionable problems fall out: a **systemic inlining gap** (§2,
the big one) and a **per-function codegen-quality gap** (§4, the steady +19%).

## 2. Root cause #1 — tcc has *no* function inliner (biggest lever)

There is **no C-function inlining pass anywhere in tcc** (the IR optimizer's only "inline"
references are inline-*asm*). `static inline` in a header is compiled as an ordinary function:
**emitted once per TU that references it, and never inlined into a call site.**

The IR operand layer (`tccir_operand.h`) is *designed* around tiny by-value struct accessors
that assume the compiler inlines them. It doesn't. Measured copies in the two binaries:

| helper (`static inline`, hot IR loops) | tcc copies | gcc copies |
|---|---|---|
| `irop_set_vreg` | **42** | 0 (fully inlined) |
| `irop_init_phys_regs` | **37** | 0 (fully inlined) |
| `irop_get_vreg` | **53** | 14 |
| `tcc_ir_op_get_src1` | **55** | 20 |
| `irop_make_imm32` | **31** | 1 |

Same function, per-function size blowups (tcc ÷ gcc): `irop_make_imm32` **49×**,
`tcc_ir_op_get_dest` **9.4×**, `tcc_ir_op_get_src2` **9.1×**, `irop_get_imm64_ex` **5.3×**,
`irop_get_vreg` **5.1×**.

This costs **twice**:
1. **CPU (the point of this exercise):** every IR operand touched during codegen pays a real
   `bl`/return + struct-by-value copy instead of a few inlined instructions. These accessors run
   per-operand, per-instruction, across the whole backend — and the backend is run by the device
   tcc on every compile.
2. **Flash:** ~226 KB of `.text` (10%) is redundant duplicated copies of 30 such helpers.
   `thop_emit` alone is **128 KB across 27 copies**; the `irop_*`/`tcc_ir_op_*` accessors add
   another ~70 KB.

The same root cause explains why several **hot lexer functions look "smaller" in tcc** in §3
(`next` 0.22×, `macro_subst_tok` 0.40×): gcc inlined their helpers *into* them (work shows up in
the caller), tcc left the helpers as out-of-line calls. It's the same missing optimization seen
from the other side — and the lexer/preprocessor is **>50% of device compile CPU** (§3), so it's
exactly where the call overhead hurts most.

## 3. Hot functions: CPU weight vs codegen size

Top of the device-representative profile (`-O0` compile, the default). `ratio` = tcc ÷ gcc size;
**<1 means gcc inlined helpers into the caller**, not that tcc is better.

```
fn                          CPU%    gccB   tccB  ratio   note
next_nomacro               24.6%    4752   4396  0.93x
macro_subst_tok            11.5%    4092   1644  0.40x   gcc inlined helpers in
tok_str_add2                8.0%     282    666  2.36x   tcc bloat
next                        6.5%    3428    764  0.22x   gcc inlined helpers in
tccpp_new                   6.5%     692    644  0.93x
macro_subst                 4.5%     364    524  1.44x
parse_btype                 3.2%    2348   3444  1.47x   tcc bloat
cstr_ccat                   2.5%      68     98  1.44x
token_lookup_cache_find     2.2%      76    108  1.42x
default_reallocator         2.2%      64    124  1.94x
post_type                   1.8%    1660   2644  1.59x
svalue_to_iroperand         1.8%    1924   2548  1.32x
sym_push                    1.4%     588   1180  2.01x
unary_funcall               1.4%   15392  20860  1.36x
```

Takeaway: **`tccpp.c` (lex + preprocess) is the CPU, by a wide margin** — `next_nomacro`,
`next`, `macro_subst_tok`, `macro_subst`, `tccpp_new`, `tok_str_add2` together are ~60% of the
profile. Whatever we do, it has to make the lexer hot path cheaper.

## 4. Root cause #2 — steady +19% per-function codegen quality

Beyond inlining, on functions where both builds emit one real copy, tcc is ~1.2–2× larger. The
gaps cluster around:
- **Dense switches over op/tag enums** compiled as linear compare chains instead of jump tables
  (`tcc_ir_op_get_*`, `thumb_generate_opcode_for_data_processing` 3.2×).
- **Repeated struct-field reloads** — weak CSE/value-numbering at the machine level means a field
  like `op->vr` is re-loaded instead of kept in a register across uses.
- **Spill-happy register allocation** in the big functions (`tcc_ir_codegen_generate` +10 KB,
  `gen_function` +5.8 KB, `unary_funcall` +5.5 KB).

This is the broad, always-on tax. Each fix is smaller per-unit than inlining but applies to the
whole binary (and to every program the device compiles).

## 5. Recommendations, ranked by expected speedup ÷ effort

1. **Inline the hot IR-operand accessors — do this first.** No new compiler pass required:
   convert the handful of hottest `static inline` helpers in `tccir_operand.h`
   (`irop_get_vreg`/`irop_set_vreg`, `irop_init_phys_regs`, `tcc_ir_op_get_src1/2/dest`,
   `irop_get_tag`, `irop_make_imm32`) into **macros** (or hand-inline at the few hottest call
   sites). tcc *will* emit macro bodies inline. Expected: removes the per-operand call+struct-copy
   overhead from the entire backend **and** reclaims a chunk of the 226 KB. Low risk, mechanical.
2. **Inline the hot lexer helpers** the same way: `cstr_ccat`, `tok_str_add2`,
   `token_lookup_cache_find`, `default_reallocator` are tiny, hot, and called in the >50%-CPU
   lexer loop. gcc inlines them; tcc can via macro-ization. Targets the single biggest CPU bucket.
3. **A minimal real inliner** (medium effort, highest ceiling): inline single-return leaf
   functions marked `inline`/`static inline` below an instruction-count threshold. This solves
   #1 and #2 generally, eliminates the 226 KB duplication, and compounds — *a tcc that inlines
   compiles a faster tcc*. Worth it if macro-ization proves too piecemeal.
4. **De-duplicate out-of-line copies** (link-time / single-definition fold). Reclaims ~226 KB
   flash but **not** the call overhead — strictly worse than inlining for speed; do it only if
   flash is the binding constraint and an inliner isn't.
5. **Jump tables for dense enum switches** in `tcc_ir_op_get_*` and the thumb opcode emitters —
   attacks the §4 +19% at its largest contributors.

The leverage multiplier worth remembering: the device tcc runs **its own compiled code**. Every
codegen improvement here makes the next self-host build of tcc itself faster, on top of speeding
up every user program it compiles.

## Reproduce

```sh
# gcc -O2 reference objects (81 TUs) -> /tmp/gcc_tcc/*.o   (see flags in this report's git history)
# per-function sizes:
arm-none-eabi-nm -S --defined-only /tmp/gcc_tcc/*.o  | awk '$3~/[tT]/{print $2,$4}' > /tmp/gcc_sizes.txt
arm-none-eabi-nm -S --defined-only bin/armv8m-tcc.elf | awk '$3~/[tT]/{print $2,$4}' > /tmp/elf_sizes.txt
# device-representative hot list:
scripts/tcc_profile.py -n 30
```
