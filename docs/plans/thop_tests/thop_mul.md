# Unit Test Plan: `thop_mul.c`

## Module
- **Source**: `arch/arm/thumb/thop_mul.c`
- **Header**: `arch/arm/thumb/thop_mul.h`
- **Category**: Multiply, divide, long multiply

## Overview
Generates MUL (T16/T32), MLA/MLS (T32), UMULL/UMLAL/SMULL/SMLAL (T32), and UDIV/SDIV (T32 with `.div=1`). T16 MUL has a custom emit and requires rd==rm (encoded as N==D). The wrapper `th_mul` manually selects T16 vs T32.

## GNU Assembly Mnemonics to Test

- `muls` (T16)
- `mul` (T32)
- `mla` (T32)
- `mls` (T32)
- `umull` (T32)
- `umlal` (T32)
- `smull` (T32)
- `smlal` (T32)
- `udiv` (T32)
- `sdiv` (T32)

## Public API
```c
thumb_opcode th_mul(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding);
thumb_opcode th_mla(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra);
thumb_opcode th_mls(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra);
thumb_opcode th_umull(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);
thumb_opcode th_umlal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);
thumb_opcode th_smull(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);
thumb_opcode th_smlal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);
thumb_opcode th_udiv(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_sdiv(uint16_t rd, uint16_t rn, uint16_t rm);
```

## Test Strategy
1. **MUL T16**: low regs, custom emit with rd==rm constraint.
2. **MUL T32**: any regs.
3. **MLA/MLS**: 4-register T32.
4. **Long multiply**: 4-register T32, custom emit.
5. **UDIV/SDIV**: requires `.div=1` feature.
6. **Wrapper logic**: `th_mul` manually routes to T16 or T32 based on reg range and encoding.

## Test Cases

### `th_mul`: T16 low reg
- **Input**: `rd=0, rn=1, rm=0, flags=NOT_IMPORTANT, enc=NONE`
- **Expected**: `{.size=2, .opcode=0x4340}` (custom emit: base=0x4340 | rd&7 | (rm&7)<<3 = 0x4340 | 0 | 0 = 0x4340)

### `th_mul`: T16 low reg (rd=2, rm=2)
- **Input**: `rd=2, rn=3, rm=2, flags=NOT_IMPORTANT, enc=NONE`
- **Expected**: `{.size=2, .opcode=0x434A}` (0x4340 | 2 | (2<<3) = 0x4340 | 2 | 0x10 = 0x4352? Wait: 2<<3 = 0x10. 0x4340 | 2 | 0x10 = 0x4352.)

### `th_mul`: T32 (high reg or enforce 32)
- **Input**: `rd=R8, rn=R1, rm=R2, flags=NOT_IMPORTANT, enc=NONE`
- **Expected**: `{.size=4, .opcode=0xFB00F002}` (base 0xFB00F000 | rd=8 | rn=1 | rm=2)

### `th_mul`: enforce 32-bit with low regs
- **Input**: `rd=0, rn=1, rm=0, flags=NOT_IMPORTANT, enc=ENFORCE_ENCODING_32BIT`
- **Expected**: T32, not T16

### `th_mla`: T32
- **Input**: `rd=R0, rn=R1, rm=R2, ra=R3`
- **Expected**: `{.size=4, .opcode=0xFB001023}` (0xFB000000 | rn=1<<16 | rd=0<<8 | ra=3<<12 | rm=2)

### `th_mls`: T32
- **Input**: `rd=R0, rn=R1, rm=R2, ra=R3`
- **Expected**: `{.size=4, .opcode=0xFB001033}`

### `th_umull`: T32
- **Input**: `rdlo=R0, rdhi=R1, rn=R2, rm=R3`
- **Expected**: `{.size=4, .opcode=0xFBA03001}` (custom emit: base 0xFBA00000 | rdhi=1<<8 | rn=2<<16 | rm=3 | rdlo=0<<12)

### `th_udiv`: requires div feature
- **Input**: `rd=R0, rn=R1, rm=R2`
- **Expected**: `{.size=4, .opcode=0xFBB0F0F2}` (base 0xFBB0F0F0 | rd=0<<8 | rn=1<<16 | rm=2)

### `th_udiv`: fails without div feature
- **Setup**: `arm_target_dependent.feat.div = 0`
- **Input**: `rd=R0, rn=R1, rm=R2`
- **Expected**: `{.size=0, .opcode=0}`

## Dependencies
- `arch/arm/thumb/thumb.c`
- `arm_target_dependent` with `.div=1` for divide tests

## Todo List
- [ ] Create `tests/unit/arm/armv8m/test_thop_mul.c`
- [ ] Add `thop_mul.c` and `thumb.c` to `UT_MODULE_SRCS`
- [ ] Initialize `arm_target_dependent.feat` with `.div=1`
- [ ] Test T16 MUL low reg custom emit
- [ ] Test T32 MUL high reg
- [ ] Test `th_mul` wrapper auto-selection (T16 vs T32)
- [ ] Test MLA/MLS 4-register encoding
- [ ] Test UMULL/UMLAL/SMULL/SMLAL custom emit
- [ ] Test UDIV/SDIV with `.div=1`
- [ ] Test UDIV/SDIV fail without `.div=1`
- [ ] Test enforce-32bit bypasses T16
- [ ] Register suite in `test_main.c`
- [ ] Verify `make ut` passes

## How to Write Tests for This Module

There are **two independent test layers** for every `thop_*` module. You should usually add both.

### 1. Unit tests — `tests/unit/arm/armv8m/`

These test the C API directly (`th_adr_imm`, `th_add_reg`, etc.) without invoking an external assembler. They run natively on the host (`gcc`) and are the fastest way to catch encoding bugs.

**File layout**
```
tests/unit/arm/armv8m/
├── test_main.c          # add UT_DECLARE_SUITE / UT_RUN_SUITE here
├── Makefile             # add source to UT_MODULE_SRCS and UT_LOCAL_SRCS
├── ut.h                 # tiny harness (UT_TEST, UT_ASSERT_EQ, UT_SUITE)
└── test_thop_xxx.c      # your new suite
```

**Minimal skeleton**
```c
#define USING_GLOBALS
#include "arch/arm/thumb/thop_xxx.h"
#include "arch/arm/thumb/thumb.h"
#include "ut.h"

static void setup_armv7m(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .feat = (thop_feat){ .t16 = 1, .t32 = 1, .it = 1, .mod_imm = 1,
                           .movw_movt = 1, .bfx = 1, .clz_rbit = 1,
                           .tbb_tbh = 1, .cbz = 1, .sat = 1, .div = 1 },
  };
}

UT_TEST(test_xxx_t1_basic)
{
  setup_armv7m();
  thumb_opcode op = th_xxx_fn(args...);
  UT_ASSERT_EQ(op.size, 2);          /* or 4 for T32 */
  UT_ASSERT_EQ(op.opcode, 0xABCD);   /* expected hex */
  return 0;
}

UT_SUITE(thop_xxx)
{
  UT_RUN(test_xxx_t1_basic);
}
```

**Computing the expected opcode**
- T1 (16-bit): `base | (rd << rd_shift) | (imm >> scale_log2)` etc.
- T3/T4 (32-bit) with **IMM_PACK_3_8_1**: call `th_packimm_3_8_1(imm)` and OR with `base | (rd << 8)`.
- T3/T4 with **IMM_PACK_CONST**: call `th_pack_const(imm)` and OR with `base | (rd << 8)`.
- **Do not forget to OR the `rd` bits** — `th_packimm_*` only returns the immediate field, not the register field.
- When in doubt, assemble the same instruction with `arm-none-eabi-as` and read the bytes:
  ```bash
  echo 'adr r8, .+0x123' | arm-none-eabi-as -march=armv8-m.main -o /dev/stdout | xxd
  ```

**Running**
```bash
cd tests/unit/arm/armv8m
make run          # build & run all suites
```

**Registering a new suite**
1. Create `test_thop_xxx.c`
2. Add it to `UT_MODULE_SRCS` (the `thop_xxx.c` under test) and `UT_LOCAL_SRCS` (the suite file) in the `Makefile`
3. Add `UT_DECLARE_SUITE(thop_xxx)` and `UT_RUN_SUITE(thop_xxx)` in `test_main.c`

---

### 2. Assembly tests — `tests/thumb/armv8m/`

These verify that TCC's inline assembler produces the **same bytes** as GNU `arm-none-eabi-as`. They are pytest-based and compare disassembly line-by-line.

**File layout**
```
tests/thumb/armv8m/
├── test_xxx.S                 # assembly source
├── data_processing_test.py    # or memory_access_test.py, etc.
│   def test_xxx():
│       utils.perform_test_for_file("test_xxx.S")
├── expected/
│   └── test_xxx               # .text-only object from GCC (auto-generated)
│   └── test_xxx_gcc          # linked ELF from GCC (auto-generated)
└── build/
    └── test_xxx              # linked ELF from TCC (auto-generated)
```

**Minimal assembly file**
```asm
.syntax unified
.thumb

.global _start
_start:

.global test_xxx
test_xxx:
    /* T1 forms */
    add  r1, r2, #3
    adds r3, r4, #1

    /* T3 forms */
    add.w r5, r6, #0xab

    /* IT blocks */
    ittee eq
    addeq  r1, r2, #3
    addeq  r3, r4, #1
    addne  r5, r6, #7
    addne  r7, r8, #9
```

**Naming conventions**
- Use `.global test_xxx` so the disassembler finds the symbol
- Label the code block `test_xxx:` (same as the filename stem)
- Each variant (T1, T2, T3) should appear at least once
- Include IT-block forms if the instruction supports conditional execution

**Running a single assembly test**
```bash
cd tests/thumb/armv8m
export TEST_CC=arm-none-eabi-gcc
export TEST_OBJDUMP=arm-none-eabi-objdump
export TEST_COMPARE_CC=arm-none-eabi-gcc
export TEST_OBJCOPY=arm-none-eabi-objcopy
python3 -m pytest data_processing_test.py::test_xxx -xvs
```

On first run, `prepare_expect()` auto-generates the `expected/` files by compiling the same `.S` with GCC. If you edit the `.S` file, delete the matching files in `expected/` so they are regenerated.

---

### 3. Agent instruction: get exact opcode from GCC

**As an agent, you MUST verify every expected opcode against GCC before writing it into a unit test.** Do not guess or manually calculate without confirmation.

**Step-by-step for a single instruction (use this in >90 % of cases):**

1. Build a one-line assembly snippet and assemble it:
   ```bash
   printf '%s\n' '.syntax unified' '.thumb' '    <your_mnemonic>' \
     | arm-none-eabi-as -march=armv8-m.main -o /tmp/t.o \
     && arm-none-eabi-objdump -d /tmp/t.o
   ```

2. Read the opcode from the `objdump` output.  
   Example output:
   ```
      0:	bf00      	nop
      2:	20ff      	movs	r0, #255	@ 0xff
   ```
   The hex value immediately after the address is the opcode in natural byte order.  
   - `bf00` → `0xBF00` (16-bit, `size = 2`)  
   - `f04f00ff` → `0xF04F00FF` (32-bit, `size = 4`)

3. Copy that value into your `UT_ASSERT_EQ(op.opcode, 0xXXXX)` assertion.

**For several instructions at once or special extensions (DSP, VFP):**

```bash
cd tests/thumb/armv8m

# default armv8-m.main, soft-float
python3 asm_encode_test.py armv8-m.main

# with DSP + VFP (for PLD, VFP, DSP, or MVE instructions)
python3 asm_encode_test.py armv8-m.main+dsp fpv5-sp-d16
```

Edit the `SAMPLE_ASM` string at the top of `asm_encode_test.py` to include your exact mnemonics, re-run, and read the **PLAIN HEX STREAM** output. The script already prints bytes in natural instruction order, so you can copy the hex directly.

**Important:** `arm-none-eabi-objdump -d` prints Thumb opcodes in natural byte order (the same order you write them in C). You do **not** need to reverse bytes.

---

### 4. Common pitfalls

| Pitfall | Fix |
|---|---|
| Forgetting `rd`/`rn` bits in 32-bit expected opcode | `base \| (rd << 8) \| th_packimm_3_8_1(imm)` |
| Forgetting imm scale in T1 | `imm >> scale_log2` (e.g. `>> 2` for word-sized) |
| Negative immediate in signed shape | Shape uses `.is_signed = true`; engine takes absolute value before packing |
| Wrong feature flags → variant skipped | Ensure `.t16 = 1` / `.t32 = 1` / `.mod_imm = 1` etc. in setup |
| PC not rejected | Add `.rd_con = REG_NOT_PC` (or `.rn_con`) to the shape |
| pytest env vars missing | Export `TEST_CC`, `TEST_OBJDUMP`, `TEST_COMPARE_CC`, `TEST_OBJCOPY` |
| `_start` multiply defined | `compile_code()` uses `-nostdlib`; do not override that |

---

### 5. Checklist for a new thop module

- [ ] Write `test_thop_xxx.c` unit suite (T1, T3, edge cases, constraint failures)
- [ ] Register suite in `test_main.c` and `Makefile`
- [ ] Write `test_xxx.S` assembly file (all encoding variants + IT blocks)
- [ ] Register pytest wrapper in the appropriate `*_test.py`
- [ ] Run unit tests: `cd tests/unit/arm/armv8m && make run`
- [ ] Run assembly test: `pytest data_processing_test.py::test_xxx -xvs`
- [ ] Verify at least one encoding with `asm_encode_test.py` or direct `arm-none-eabi-as`
