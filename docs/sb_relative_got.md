# SB-relative GOT addressing

Under `text_and_data_separation` + PIC, R9 holds the GOT base at runtime (the
static base, "SB"). Taking the address of a global used to cost four words:

```
ldr   rX, [pc, #lit]    4 B   GOT slot offset, relocated R_ARM_GOT32
add   rX, r9            2 B
ldr   rX, [rX]          4 B
+ literal pool word     4 B
```

`-msb-relative-got` (default on for YASOS targets) collapses that to one
instruction, because the GOT slot offset fits the imm12 of a wide LDR:

```
ldr.w rX, [r9, #imm12]  4 B   relocated R_ARM_GOT_SBREL12
```

## The pieces

| Site | Role |
|------|------|
| `elf.h` | `R_ARM_GOT_SBREL12` = 138, a tcc-private value next to `R_ARM_RODATA_OFF` (137) |
| `arm-thumb-gen.c` `load_full_const` | Emits the wide LDR and the relocation, ahead of the literal-pool path |
| `arm-thumb-gen.c` `th_pic_reloc_for_sym` | The relocation choice, shared by both paths |
| `arm-link.c` | `code_reloc` (data), `gotplt_entry_type` (`ALWAYS_GOTPLT_ENTRY`), `relocate` (patch imm12) |
| `tccyaff.c` | Statically resolved — no runtime YAFF relocation |

The GOT slot itself is unaffected: it is still filled via `GLOB_DAT`/`JUMP_SLOT`
and relocated by the dynamic loader. Only the code-side reference changes.

## Two invariants worth knowing

**The relocation choice must be identical in the dry-run and real passes.**
Codegen runs twice and the two passes must agree on every instruction size, so
`th_pic_reloc_for_sym` is the single source of truth for both paths. Its one
registration-dependent input is `sym_off`, and it is stable: an unregistered
symbol yields `sym_off == 0 == SHN_UNDEF`, exactly what a registered-but-
undefined symbol yields, so registration only shifts the value when a symbol
becomes *defined* between passes — impossible inside one `gen_function`.

**The emit goes through `ot_check`, not `ot_check_ldr_imm`.** The latter has a
redundant-reload cache keyed on `(rt, rn, imm)`. Every SB load looks like
`[r9,#0]` until the linker patches it, so that cache would conflate distinct
symbols and elide real loads. Only STR is recorded in that cache, so emitting
through plain `ot_check` is safe.

## The cliff

imm12 caps the GOT at 4096 bytes. Exceeding it is a hard link error naming the
symbol and the remedy:

```
tcc: error: GOT slot for 'g508' at offset 4096 exceeds the 4096-byte
SB-relative range; rebuild this module with -mno-sb-relative-got
```

The offset is assigned by the linker, so there is no in-band fallback — the
literal-pool path stays selectable per build via `-mno-sb-relative-got` instead.
As with tcc's other `relocate()` errors, a failed link still leaves an output
file behind; the non-zero exit status is the reliable signal.

Largest module today is the on-device `tcc` at **2328 B of 4096 (57%)**, about
1.8x headroom. `-mno-sb-relative-got` is also set for `tests/ir_tests` — those
images are bare metal, with no loader to establish an R9 GOT base.

## Measured effect

Clean A/B over the whole rootfs (29 modules), same tree, default flipped:

| | SB off | SB on | Δ |
|---|---|---|---|
| rootfs total | 2,907,864 | 2,852,840 | **−55,024 B (−1.89%)** |
| `usr/bin/tcc` | 2,119,200 | 2,084,840 | −34,360 B (−1.62%) |
| `usr/games/hmm` | 115,640 | 102,760 | −12,880 B (−11.14%) |
| `usr/bin/toybox` | 241,056 | 234,880 | −6,176 B (−2.56%) |

14,002 SB-relative loads in the on-device tcc.

Note this is roughly half of a naive `10 B x sites` projection, for two reasons:
the old sequence often used narrow encodings (6 B, not 10 B), and
`th_literal_pool_find_or_allocate` shared one pool word across all references to
the same symbol within a function. The instruction-count win on the execution
path is the more durable argument: two fewer executed instructions and one fewer
literal-pool data fetch per global access.
