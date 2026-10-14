# The release path of `kheap_unlock` materialises a known zero twice

**Status:** open · **Severity:** performance · **Found:** 2026-10-12 (20:45 kernel_compare battery, per-PC attribution, disassembly)

## Current state

`memory.heap.kheap_lock.yasos_kheap_unlock` currently compiles to (YasOS cross, kernel flags,
`repro/kernel-noreturn-call-cfg-fallthrough.c`):

```
ldr  r0, =lock
ldr  r1, [r0, #4]        ; depth
cbnz r1, 1f
  ldr r0,=msg; movs r1,#42; mvn.w r2,#0x55555555; movs r3,#1
  push {r3, lr}          ; cold block only
  bl  lang_panic
1: subs r2, r1, #1
str  r2, [r0, #4]
bne.w 2f
ldr  r1, [r0, #8]; movs r2,#0; str r2,[r0,#8]; movs r2,#0; stl r2,[r0]; sev; msr PRIMASK,r1
2: bx lr
```

7 instructions on the nested path and 13 on the release path; gcc -O2 has 7 and 12.

## What remains

| Waste | /call | ≈ instr. | Cause |
| --- | ---: | ---: | --- |
| second `movs r2,#0` on the release path | 0.5 | 6.7k | no reuse of a known value |

**Known-zero reuse.** On the fall-through of `subs r2,r1,#1; bne`, `r2 == 0`, and gcc
stores that register. tcc materialises `#0` for each of the two stores
(`outer_flags = 0` and the inlined atomic release store). Even reusing the first
`movs r2,#0` would save one: `ir/codegen.c` (`tcc_ir_codegen_generate`, the per-instruction
cache maintenance) fully resets `imm_cache` at every FUNCCALL, including the
`__tcc_ax_stl*` call that `tcc_ir_call_clobbers_nothing` says writes no register, so
`thumb_inline_atomic_call` starts with an empty cache. A fix there must keep the cache
sound inside the inline atomics' LDREX/STREX retry loops (a constant reused from before
the loop must not live in a register the loop body writes).

Related, seen while reducing (not in the kernel body): `p->depth = p->depth - 1;
return p->depth;` keeps `str r2,[r0,#4]; ldr r0,[r0,#4]`: SSA `load_cse` does not
forward a STORE_INDEXED value to a following LOAD_INDEXED of the same base/offset.

## LLM reporting name

kernel-noreturn-call-cfg-fallthrough

## Reproducer

`repro/kernel-noreturn-call-cfg-fallthrough.c` (compile command in its header);
`memory_heap_kheap_lock_yasos_kheap_unlock__7693` shows the remaining item.
