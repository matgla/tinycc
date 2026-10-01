# Folding `x < INT32_MIN`-style compares in gen_opic breaks the self-hosted tcc

**Status:** open · **Severity:** correctness (self-host miscompile, latent) · **Found:** 2026-09-29 (yasos QEMU smoke, zig-abi a9f76c02 + the hunk below)

## Summary

A frontend constant fold that is correct by itself -- in `gen_opic`
(source/frontend/gen/op/int.c), fold `x <u 0`, `x >=u 0`, `x >u UINT_MAX`,
`x <=u UINT_MAX`, `x <s INT_MIN`, `x >=s INT_MIN`, `x >s INT_MAX`, `x <=s INT_MAX`
(32- and 64-bit) to 0/1 when x is not a volatile read and not a pending
VT_CMP/VT_JMP -- makes the **native** tcc that the cross builds from tinycc's
own sources miscompile ordinary programs on the device:

- `ir_tests/bug_ternary_switch.c` prints `test1: 1 FAIL` (expected `test1: 6 PASS`)
  at **every** -O level of the native compiler, -O0 included;
- ~1,000 of the 14,104 QEMU smoke tests fail (IR tests at all levels), and the
  run eventually hangs.

The host cross with the same fold passes the whole IR suite (14,692) and GCC
torture (11,309), so the folded *code it produces for test programs* is right;
what breaks is the native tcc binary itself (built at -O1 by the cross).
Reverting just this hunk (keeping everything else) makes the device suite pass.

## Suspicion

The fold turns more conditions into compile-time constants.  Constant
conditions go through tcc's nocode_wanted / CODE_OFF machinery (dead-branch
suppression); bug_ternary_switch itself is the regression test for a
CODE_OFF_BIT leak through expr_cond/backpatch.  A tinycc source construct
where one of these compares is now constant probably reaches a path that
drops live code.  Not investigated.

## Repro

1. Apply the hunk below to source/frontend/gen/op/int.c (it goes in
   gen_opic, right after the relational-reversal block).
2. `./build_rootfs.sh -o rootfs.img && rm -rf .zig-cache && zig build -Doptimize=ReleaseSafe`
3. `./scripts/run_qemu_smoke.sh --no-build tcc_suite_test.py -k bug_ternary_switch` -> 3 failed.

Next step: bisect which tinycc TU/function the fold changes (build the native
tcc with gcc objects mixed in, per docs/selfhost_miscompile_debugging.md), then
find the compare that became constant there.

## The hunk

```diff
@@ -260,7 +260,42 @@ void gen_opic(int op)
         l1 = 0;
       }
     }
-    if (c1 && ((l1 == 0 && (op == TOK_SHL || op == TOK_SHR || op == TOK_SAR)) || (l1 == -1 && op == TOK_SAR)))
+    /* x compared with an end of its own type's range -- `x < INT_MIN`,
+     * `x > UINT_MAX`, `x >= 0u` and their mirrors -- holds or fails for every
+     * x.  zig.h's overflow wrappers test the wrapped result against
+     * zig_minInt/zig_maxInt of the type's full width that way after every
+     * __builtin_*_overflow, and left alone each is a CMP (one through the
+     * literal pool) and a branch.  x is dropped only when that drops nothing:
+     * not a volatile read, not a pending compare or jump chain. */
+    int taut = -1;
+    if (c2 && !c1 && t1 == t2 && (t1 == VT_INT || t1 == VT_LLONG) && !(v1->type.t & VT_VOLATILE) &&
+        (v1->r & VT_VALMASK) != VT_CMP && (v1->r & VT_VALMASK) != VT_JMP && (v1->r & VT_VALMASK) != VT_JMPI)
+    {
+      const int is64 = t1 == VT_LLONG;
+      const uint64_t umax = is64 ? ~(uint64_t)0 : 0xffffffffull;
+      const uint64_t smin = is64 ? (uint64_t)1 << 63 : 0xffffffff80000000ull;
+      const uint64_t smax = is64 ? ~(uint64_t)0 >> 1 : 0x7fffffffull;
+      switch (op)
+      {
+      case TOK_ULT: taut = l2 == 0 ? 0 : -1; break;
+      case TOK_UGE: taut = l2 == 0 ? 1 : -1; break;
+      case TOK_UGT: taut = l2 == umax ? 0 : -1; break;
+      case TOK_ULE: taut = l2 == umax ? 1 : -1; break;
+      case TOK_LT: taut = l2 == smin ? 0 : -1; break;
+      case TOK_GE: taut = l2 == smin ? 1 : -1; break;
+      case TOK_GT: taut = l2 == smax ? 0 : -1; break;
+      case TOK_LE: taut = l2 == smax ? 1 : -1; break;
+      default: break;
+      }
+    }
+    if (taut >= 0)
+    {
+      vtop->c.i = taut;
+      vswap();
+      vtop--;
+      vtop->r |= VT_NONCONST;
+    }
+    else if (c1 && ((l1 == 0 && (op == TOK_SHL || op == TOK_SHR || op == TOK_SAR)) || (l1 == -1 && op == TOK_SAR)))
     {
       /* treat (0 << x), (0 >> x) and (-1 >> x) as constant */
       vpop();
```
