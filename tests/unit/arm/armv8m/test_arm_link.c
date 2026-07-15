/*
 *  test_arm_link.c - suite for arm-link.c (ELF relocation / GOT-PLT backend)
 *
 *  Covers:
 *    - code_reloc(): classifies a relocation type as code(1)/data(0)/
 *      unknown(-1). Table-tested over every case label in the switch, plus
 *      a couple of R_ARM_* values that intentionally fall outside both
 *      case lists (the -1 default path).
 *    - gotplt_entry_type(): same shape, four buckets (NO/BUILD_GOT_ONLY/
 *      AUTO/ALWAYS), table-tested over every case label plus a couple of
 *      unmatched values.
 *    - write_thumb_instruction(): the byte-level opcode encoder. 16-bit and
 *      32-bit opcodes write the expected little-endian halfwords; an invalid
 *      `size` (neither 2 nor 4) is a documented no-op (buffer left
 *      untouched) -- see test_write_thumb_instruction_invalid_size_is_noop.
 *    - relocate(): the big per-relocation-type value patcher. Table/case
 *      tests for the common ARM/Thumb-2 relocation families: PC24/CALL
 *      (BL/BLX), MOVW/MOVT ABS pairs (ARM + Thumb-2), MOVW/MOVT PREL,
 *      ABS32/REL32, GOTPC/GOTOFF/RODATA_OFF, GOT32/GOT_PREL, COPY/NONE/
 *      RELATIVE (no-ops), V4BX, GLOB_DAT/JUMP_SLOT, PREL31, and the
 *      Thumb-2 branch-family encodings (THM_JUMP19, THM_PC22/JUMP24,
 *      THM_JUMP6, THM_PC12, THM_PC8, THM_ALU_PREL_11_0).
 *
 *  HARNESS NOTES:
 *  relocate() takes an explicit `TCCState *s1` parameter and arm-link.c is
 *  compiled without `USING_GLOBALS`, so every `s1->field`-style access in
 *  tcc.h's TCC_STATE_VAR()/qrel macros resolves against whatever TCCState
 *  we pass in -- there is no dependency on the shared `tcc_state` global
 *  used by USING_GLOBALS suites elsewhere in this binary. This file builds
 *  a small local TCCState + Section fixtures per test instead.
 *
 *  Per the Makefile's current UT_COVERAGE_ONLY_SRCS split, tccelf.c/libtcc.c
 *  are compiled for coverage bookkeeping but *not* linked into the UT
 *  binary, so relocate()'s link-time dependencies on get_sym_attr(),
 *  write16le(), add32le(), _tcc_error_noabort() and tcc_enter_state() are
 *  not otherwise satisfied. This file provides minimal local stubs for
 *  those five symbols (byte-identical little-endian semantics for the
 *  write16le/add32le helpers; a message-recording, non-aborting stub for
 *  the error path; a real-but-trivial sym_attr grower for get_sym_attr()
 *  mirroring tccelf.c's algorithm since relocate()'s GOT-offset reads
 *  depend on its actual growth semantics). create_plt_entry()/relocate_plt()
 *  are NOT exercised here: they need arm_init()/arm_target_dependent (real
 *  impl in arm-thumb-gen.c, deliberately not linked -- see Makefile
 *  comment) plus a populated PLT section, which would mean rebuilding a
 *  chunk of the real linker fixture graph. Out of scope per task instructions.
 */

#include "source/backend/arch/arm/thumb/thumb.h"
#include "tcc.h"

#include "ut.h"

/* ------------------------------------------------------------------ */
/* Link-stub layer (see HARNESS NOTES above).                          */
/* ------------------------------------------------------------------ */

/* Same little-endian semantics as tcctools.c's write16le(). */
void write16le(unsigned char *p, uint16_t x)
{
  p[0] = (unsigned char)(x & 0xff);
  p[1] = (unsigned char)((x >> 8) & 0xff);
}

/* Same semantics as tcctools.c's add32le(): read-modify-write via the
 * already-linked read32le()/write32le() (stubs.c / codegen_mop_stubs.c). */
void add32le(unsigned char *p, int32_t x)
{
  write32le(p, read32le(p) + (uint32_t)x);
}

/* Minimal, non-aborting stand-in for libtcc.c's _tcc_error_noabort(): record
 * that relocate() hit an out-of-range/error path (tests assert on the flag,
 * not the message) without pulling in error1()/longjmp/nb_stk_data. */
static int ut_arm_link_error_calls;
static char ut_arm_link_last_error[256];

int _tcc_error_noabort(const char *fmt, ...)
{
  va_list ap;
  ut_arm_link_error_calls++;
  va_start(ap, fmt);
  vsnprintf(ut_arm_link_last_error, sizeof(ut_arm_link_last_error), fmt, ap);
  va_end(ap);
  return -1;
}

/* tcc_error_noabort() (tcc.h macro) is TCC_SET_STATE(_tcc_error_noabort) in
 * non-USING_GLOBALS mode, i.e. `(tcc_enter_state(s1), _tcc_error_noabort)`;
 * the multi-threaded compile-serialization semantics don't matter here. */
void tcc_enter_state(TCCState *s1)
{
  (void)s1;
}

/* Real algorithm from tccelf.c's get_sym_attr(): grow sym_attrs to the next
 * power of two >= index and zero the new tail. relocate()'s R_ARM_GOT32/
 * R_ARM_GOT_PREL/R_ARM_ABS32(dyn) cases read real fields off the returned
 * pointer, so this needs to be functionally faithful, not a fixed stub. */
struct sym_attr *get_sym_attr(TCCState *s1, int index, int alloc)
{
  int n;
  struct sym_attr *tab;

  if (index >= s1->nb_sym_attrs)
  {
    if (!alloc)
      return s1->sym_attrs;
    n = 1;
    while (index >= n)
      n *= 2;
    tab = tcc_realloc(s1->sym_attrs, n * sizeof(*s1->sym_attrs));
    s1->sym_attrs = tab;
    memset(s1->sym_attrs + s1->nb_sym_attrs, 0, (n - s1->nb_sym_attrs) * sizeof(*s1->sym_attrs));
    s1->nb_sym_attrs = n;
  }
  return &s1->sym_attrs[index];
}

/* write_thumb_instruction() has no tcc.h declaration (only defined in
 * arm-link.c, called from the NEED_BUILD_GOT-gated relocate_plt()). */
void write_thumb_instruction(uint8_t *p, thumb_opcode op);

/* ------------------------------------------------------------------ */
/* Fixture helpers                                                     */
/* ------------------------------------------------------------------ */

/* Builds a minimal TCCState with a real symtab_section (one symbol table
 * entry, index 0 = the traditional null sym, index 1 = a usable symbol) so
 * relocate()'s `sym = &symtab[ELFW(R_SYM)(rel->r_info)]` lookup is valid.
 * Also zeroes error-stub bookkeeping so each test starts clean. */
static void ut_arm_link_reset(TCCState *s1, Section *symtab_sec,
                              Elf32_Sym *syms, int nsyms)
{
  memset(s1, 0, sizeof(*s1));
  memset(symtab_sec, 0, sizeof(*symtab_sec));
  memset(syms, 0, sizeof(*syms) * (size_t)nsyms);
  symtab_sec->data = (unsigned char *)syms;
  /* tcc.h #defines the bare identifier `symtab_section` to `s1->symtab_section`
   * (non-USING_GLOBALS mode) -- writing `s1->symtab_section` here would
   * double-expand to `s1->s1->symtab_section`, so assign through the macro
   * name itself, exactly like tccelf.c does. */
  symtab_section = symtab_sec;
  ut_arm_link_error_calls = 0;
  ut_arm_link_last_error[0] = 0;
}

static ElfW_Rel rel_for_sym(int sym_index, int reloc_type)
{
  ElfW_Rel rel;
  rel.r_offset = 0;
  rel.r_info = ELFW(R_INFO)(sym_index, reloc_type);
  return rel;
}

/* ------------------------------------------------------------------ */
/* code_reloc()                                                        */
/* ------------------------------------------------------------------ */

UT_TEST(test_code_reloc_data_relocations)
{
  static const int data_types[] = {
      R_ARM_MOVT_ABS, R_ARM_MOVW_ABS_NC, R_ARM_THM_MOVT_ABS, R_ARM_THM_MOVW_ABS_NC,
      R_ARM_ABS32, R_ARM_REL32, R_ARM_GOTPC, R_ARM_GOTOFF, R_ARM_RODATA_OFF,
      R_ARM_GOT32, R_ARM_GOT_PREL, R_ARM_COPY, R_ARM_GLOB_DAT, R_ARM_NONE,
      R_ARM_TARGET1, R_ARM_MOVT_PREL, R_ARM_MOVW_PREL_NC};
  for (size_t i = 0; i < sizeof(data_types) / sizeof(data_types[0]); i++)
    UT_ASSERT_EQ(code_reloc(data_types[i]), 0);
  return 0;
}

UT_TEST(test_code_reloc_code_relocations)
{
  static const int code_types[] = {
      R_ARM_PC24, R_ARM_CALL, R_ARM_JUMP24, R_ARM_PLT32, R_ARM_THM_PC22,
      R_ARM_THM_JUMP24, R_ARM_THM_JUMP19, R_ARM_PREL31, R_ARM_V4BX,
      R_ARM_JUMP_SLOT, R_ARM_THM_ALU_PREL_11_0, R_ARM_THM_JUMP6,
      R_ARM_THM_PC12, R_ARM_THM_PC8};
  for (size_t i = 0; i < sizeof(code_types) / sizeof(code_types[0]); i++)
    UT_ASSERT_EQ(code_reloc(code_types[i]), 1);
  return 0;
}

UT_TEST(test_code_reloc_unknown_relocations)
{
  /* R_ARM_RELATIVE (23) and R_ARM_TARGET2 (41) sit between/outside the two
   * case lists -- neither a code() nor a data() reloc as far as this switch
   * is concerned. */
  UT_ASSERT_EQ(code_reloc(R_ARM_RELATIVE), -1);
  UT_ASSERT_EQ(code_reloc(R_ARM_TARGET2), -1);
  UT_ASSERT_EQ(code_reloc(9999), -1);
  return 0;
}

/* ------------------------------------------------------------------ */
/* gotplt_entry_type()                                                  */
/* ------------------------------------------------------------------ */

UT_TEST(test_gotplt_entry_type_no_entry)
{
  static const int types[] = {R_ARM_NONE, R_ARM_COPY, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT};
  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
    UT_ASSERT_EQ(gotplt_entry_type(types[i]), NO_GOTPLT_ENTRY);
  return 0;
}

UT_TEST(test_gotplt_entry_type_auto_entry)
{
  static const int types[] = {
      R_ARM_PC24, R_ARM_CALL, R_ARM_JUMP24, R_ARM_PLT32, R_ARM_THM_PC22,
      R_ARM_THM_ALU_PREL_11_0, R_ARM_THM_JUMP6, R_ARM_THM_JUMP19,
      R_ARM_THM_JUMP24, R_ARM_MOVT_ABS, R_ARM_MOVW_ABS_NC, R_ARM_THM_MOVT_ABS,
      R_ARM_THM_MOVW_ABS_NC, R_ARM_PREL31, R_ARM_ABS32, R_ARM_REL32,
      R_ARM_V4BX, R_ARM_TARGET1, R_ARM_MOVT_PREL, R_ARM_MOVW_PREL_NC,
      R_ARM_THM_PC12, R_ARM_THM_PC8};
  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
    UT_ASSERT_EQ(gotplt_entry_type(types[i]), AUTO_GOTPLT_ENTRY);
  return 0;
}

UT_TEST(test_gotplt_entry_type_build_got_only)
{
  static const int types[] = {R_ARM_GOTPC, R_ARM_GOTOFF, R_ARM_RODATA_OFF};
  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
    UT_ASSERT_EQ(gotplt_entry_type(types[i]), BUILD_GOT_ONLY);
  return 0;
}

UT_TEST(test_gotplt_entry_type_always_entry)
{
  UT_ASSERT_EQ(gotplt_entry_type(R_ARM_GOT32), ALWAYS_GOTPLT_ENTRY);
  UT_ASSERT_EQ(gotplt_entry_type(R_ARM_GOT_PREL), ALWAYS_GOTPLT_ENTRY);
  return 0;
}

UT_TEST(test_gotplt_entry_type_unknown)
{
  UT_ASSERT_EQ(gotplt_entry_type(R_ARM_RELATIVE), -1);
  UT_ASSERT_EQ(gotplt_entry_type(R_ARM_TARGET2), -1);
  UT_ASSERT_EQ(gotplt_entry_type(9999), -1);
  return 0;
}

/* ------------------------------------------------------------------ */
/* write_thumb_instruction()                                           */
/* ------------------------------------------------------------------ */

UT_TEST(test_write_thumb_instruction_16bit)
{
  uint8_t buf[4] = {0xAA, 0xAA, 0xAA, 0xAA};
  thumb_opcode op;
  op.size = 2;
  op.opcode = 0x4770; /* bx lr */
  write_thumb_instruction(buf, op);
  /* little-endian halfword */
  UT_ASSERT_EQ(buf[0], 0x70);
  UT_ASSERT_EQ(buf[1], 0x47);
  /* untouched beyond the 16-bit opcode */
  UT_ASSERT_EQ(buf[2], 0xAA);
  UT_ASSERT_EQ(buf[3], 0xAA);
  return 0;
}

UT_TEST(test_write_thumb_instruction_32bit)
{
  uint8_t buf[4] = {0, 0, 0, 0};
  thumb_opcode op;
  op.size = 4;
  op.opcode = 0xF000E000u; /* hi halfword 0xF000, lo halfword 0xE000 */
  write_thumb_instruction(buf, op);
  /* First halfword written is opcode >> 16 = 0xF000. */
  UT_ASSERT_EQ(buf[0], 0x00);
  UT_ASSERT_EQ(buf[1], 0xF0);
  /* Second halfword written is opcode & 0xffff = 0xE000. */
  UT_ASSERT_EQ(buf[2], 0x00);
  UT_ASSERT_EQ(buf[3], 0xE0);
  return 0;
}

UT_TEST(test_write_thumb_instruction_invalid_size_is_noop)
{
  uint8_t buf[4] = {0x11, 0x22, 0x33, 0x44};
  thumb_opcode op;
  op.size = 3; /* neither 2 nor 4 */
  op.opcode = 0xdeadbeef;
  write_thumb_instruction(buf, op);
  UT_ASSERT_EQ(buf[0], 0x11);
  UT_ASSERT_EQ(buf[1], 0x22);
  UT_ASSERT_EQ(buf[2], 0x33);
  UT_ASSERT_EQ(buf[3], 0x44);
  return 0;
}

UT_TEST(test_write_thumb_instruction_zero_size_is_noop)
{
  uint8_t buf[4] = {0x99, 0x88, 0x77, 0x66};
  thumb_opcode op;
  op.size = 0;
  op.opcode = 0x1234;
  write_thumb_instruction(buf, op);
  UT_ASSERT_EQ(buf[0], 0x99);
  UT_ASSERT_EQ(buf[1], 0x88);
  UT_ASSERT_EQ(buf[2], 0x77);
  UT_ASSERT_EQ(buf[3], 0x66);
  return 0;
}

/* ------------------------------------------------------------------ */
/* relocate(): common code-branch relocations (BL/BLX)                 */
/* ------------------------------------------------------------------ */

UT_TEST(test_relocate_pc24_forward_arm_call)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  /* ARM-mode BL encoding: cond=1110(E), 101(BL)=0xEB, 24-bit signed word
   * offset field, all zero here (the field is cleared and recomputed). */
  uint8_t buf[4];
  write32le(buf, 0xEB000000u);

  addr_t addr = 0x1000;
  addr_t val = 0x1010; /* target 16 bytes ahead, ARM (not thumb) */
  ElfW_Rel rel = rel_for_sym(1, R_ARM_PC24);
  relocate(s1, &rel, R_ARM_PC24, buf, addr, val);

  /* x = val - addr = 0x10 -> encoded as (x>>2) = 4 in the low 24 bits;
   * top byte (cond+opcode) preserved as 0xEB. */
  uint32_t result = read32le(buf);
  UT_ASSERT_EQ(result >> 24, 0xEB);
  UT_ASSERT_EQ(result & 0xffffff, 4);
  UT_ASSERT_EQ(ut_arm_link_error_calls, 0);
  return 0;
}

UT_TEST(test_relocate_pc24_out_of_range_reports_error)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0xEB000000u);

  /* Offset far outside the +-32MB signed 26-bit branch range. */
  addr_t addr = 0;
  addr_t val = 0x10000000u;
  ElfW_Rel rel = rel_for_sym(1, R_ARM_CALL);
  relocate(s1, &rel, R_ARM_CALL, buf, addr, val);

  UT_ASSERT(ut_arm_link_error_calls >= 1);
  return 0;
}

/* ------------------------------------------------------------------ */
/* relocate(): R_ARM_MOVW_ABS_NC / R_ARM_MOVT_ABS                      */
/* ------------------------------------------------------------------ */

/* Regression lock for bugs.md #10 (fixed): arm-link.c's R_ARM_MOVT_ABS/
 * R_ARM_MOVW_ABS_NC case used to test `if (type == R_ARM_THM_MOVT_ABS)` to
 * choose between an OR-merge and add32le. R_ARM_THM_MOVT_ABS is a *different*
 * case label (handled separately) and can never equal `type` inside this
 * block, so that branch was dead code; the ARM (A32) MOVW/MOVT relocations
 * always used add32le. The dead conditional has been removed (matching
 * upstream tinycc, which just does add32le here). This pins that behavior. */
UT_TEST(test_relocate_movw_abs_nc_uses_add32le)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0); /* start from a zeroed immediate field */

  addr_t val = 0x12345678u;
  ElfW_Rel rel = rel_for_sym(1, R_ARM_MOVW_ABS_NC);
  relocate(s1, &rel, R_ARM_MOVW_ABS_NC, buf, 0, val);

  /* imm12 = val & 0xfff, imm4 = (val>>12) & 0xf, x = imm4<<16 | imm12 */
  uint32_t imm12 = val & 0xfffu;
  uint32_t imm4 = (val >> 12) & 0xfu;
  uint32_t expect_x = (imm4 << 16) | imm12;
  UT_ASSERT_EQ(read32le(buf), expect_x);
  return 0;
}

UT_TEST(test_relocate_movt_abs_shifts_value_right_16)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0);

  addr_t val = 0x89ABCDEFu;
  ElfW_Rel rel = rel_for_sym(1, R_ARM_MOVT_ABS);
  relocate(s1, &rel, R_ARM_MOVT_ABS, buf, 0, val);

  uint32_t hi = val >> 16; /* 0x89AB */
  uint32_t imm12 = hi & 0xfffu;
  uint32_t imm4 = (hi >> 12) & 0xfu;
  uint32_t expect_x = (imm4 << 16) | imm12;
  UT_ASSERT_EQ(read32le(buf), expect_x);
  return 0;
}

/* ------------------------------------------------------------------ */
/* relocate(): R_ARM_THM_MOVW_ABS_NC / R_ARM_THM_MOVT_ABS               */
/* ------------------------------------------------------------------ */

UT_TEST(test_relocate_thm_movw_abs_nc_or_merges_into_existing_bits)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  /* THM_MOVW_ABS_NC takes the `else` (add32le) branch: type is
   * R_ARM_THM_MOVW_ABS_NC, not R_ARM_THM_MOVT_ABS. */
  uint8_t buf[4];
  write32le(buf, 0);

  addr_t val = 0x0000ABCDu;
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_MOVW_ABS_NC);
  relocate(s1, &rel, R_ARM_THM_MOVW_ABS_NC, buf, 0, val);

  uint32_t imm8 = val & 0xffu;
  uint32_t imm3 = (val >> 8) & 0x7u;
  uint32_t i = (val >> 11) & 1u;
  uint32_t imm4 = (val >> 12) & 0xfu;
  uint32_t expect_x = (imm3 << 28) | (imm8 << 16) | (i << 10) | imm4;
  UT_ASSERT_EQ(read32le(buf), expect_x);
  return 0;
}

UT_TEST(test_relocate_thm_movt_abs_or_merges_into_existing_bits)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  /* Pre-existing opcode bits that must be preserved by the OR-merge. */
  uint8_t buf[4];
  write32le(buf, 0x00010001u);

  addr_t val = 0xABCD1234u;
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_MOVT_ABS);
  relocate(s1, &rel, R_ARM_THM_MOVT_ABS, buf, 0, val);

  uint32_t hi = val >> 16; /* 0xABCD */
  uint32_t imm8 = hi & 0xffu;
  uint32_t imm3 = (hi >> 8) & 0x7u;
  uint32_t i = (hi >> 11) & 1u;
  uint32_t imm4 = (hi >> 12) & 0xfu;
  uint32_t expect_x = (imm3 << 28) | (imm8 << 16) | (i << 10) | imm4;
  UT_ASSERT_EQ(read32le(buf), (0x00010001u | expect_x));
  return 0;
}

/* ------------------------------------------------------------------ */
/* relocate(): R_ARM_MOVT_PREL / R_ARM_MOVW_PREL_NC                     */
/* ------------------------------------------------------------------ */

UT_TEST(test_relocate_movw_prel_nc_roundtrip_zero_addend)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0); /* addend fields all zero -> addend == 0 */

  addr_t addr = 0x2000;
  addr_t val = 0x2000; /* val - addr == 0 after adding the zero addend */
  ElfW_Rel rel = rel_for_sym(1, R_ARM_MOVW_PREL_NC);
  relocate(s1, &rel, R_ARM_MOVW_PREL_NC, buf, addr, val);

  /* val stays 0 -> imm12/imm4 fields are both zero. */
  UT_ASSERT_EQ(read32le(buf), 0u);
  return 0;
}

/* ------------------------------------------------------------------ */
/* relocate(): R_ARM_ABS32 / R_ARM_REL32                                */
/* ------------------------------------------------------------------ */

UT_TEST(test_relocate_abs32_non_dyn_adds_value)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  s1->output_type = TCC_OUTPUT_EXE; /* not TCC_OUTPUT_DYN -> skip qrel path */

  uint8_t buf[4];
  write32le(buf, 0x100);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_ABS32);
  relocate(s1, &rel, R_ARM_ABS32, buf, 0, 0x42);

  UT_ASSERT_EQ(read32le(buf), 0x142u);
  return 0;
}

UT_TEST(test_relocate_rel32_subtracts_addr)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_REL32);
  relocate(s1, &rel, R_ARM_REL32, buf, 0x1000, 0x1040);

  UT_ASSERT_EQ(read32le(buf), 0x40u);
  return 0;
}

/* ------------------------------------------------------------------ */
/* relocate(): GOT-relative families                                    */
/* ------------------------------------------------------------------ */

UT_TEST(test_relocate_gotpc)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec, got_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  memset(&got_sec, 0, sizeof(got_sec));
  got_sec.sh_addr = 0x3000;
  s1->got = &got_sec;

  uint8_t buf[4];
  write32le(buf, 0);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_GOTPC);
  relocate(s1, &rel, R_ARM_GOTPC, buf, 0x2000, 0);

  UT_ASSERT_EQ(read32le(buf), (uint32_t)(0x3000 - 0x2000));
  return 0;
}

UT_TEST(test_relocate_gotoff)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec, got_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  memset(&got_sec, 0, sizeof(got_sec));
  got_sec.sh_addr = 0x4000;
  s1->got = &got_sec;

  uint8_t buf[4];
  write32le(buf, 0);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_GOTOFF);
  relocate(s1, &rel, R_ARM_GOTOFF, buf, 0, 0x4020);

  UT_ASSERT_EQ(read32le(buf), 0x20u);
  return 0;
}

UT_TEST(test_relocate_rodata_off)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec, rodata_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  memset(&rodata_sec, 0, sizeof(rodata_sec));
  rodata_sec.sh_addr = 0x5000;
  /* Assign via the bare macro name -- see ut_arm_link_reset() comment on
   * `symtab_section` for why `s1->rodata_section` would double-expand. */
  rodata_section = &rodata_sec;

  uint8_t buf[4];
  write32le(buf, 0);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_RODATA_OFF);
  relocate(s1, &rel, R_ARM_RODATA_OFF, buf, 0, 0x5010);

  UT_ASSERT_EQ(read32le(buf), 0x10u);
  return 0;
}

UT_TEST(test_relocate_got32_writes_sym_got_offset)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  /* Pre-populate the sym_attr's got_offset via get_sym_attr(alloc=1). */
  struct sym_attr *attr = get_sym_attr(s1, 1, 1);
  attr->got_offset = 0x18;

  uint8_t buf[4];
  write32le(buf, 0xFFFFFFFFu);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_GOT32);
  relocate(s1, &rel, R_ARM_GOT32, buf, 0, 0);

  UT_ASSERT_EQ(read32le(buf), 0x18u);
  tcc_free(s1->sym_attrs);
  return 0;
}

UT_TEST(test_relocate_got_prel_writes_pc_relative_got_offset)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec, got_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  memset(&got_sec, 0, sizeof(got_sec));
  got_sec.sh_addr = 0x6000;
  s1->got = &got_sec;

  struct sym_attr *attr = get_sym_attr(s1, 1, 1);
  attr->got_offset = 0x20;

  uint8_t buf[4];
  write32le(buf, 0);

  addr_t addr = 0x6100;
  ElfW_Rel rel = rel_for_sym(1, R_ARM_GOT_PREL);
  relocate(s1, &rel, R_ARM_GOT_PREL, buf, addr, 0);

  /* got->sh_addr + got_offset - addr - 8 */
  uint32_t expect = (uint32_t)(0x6000 + 0x20 - 0x6100 - 8);
  UT_ASSERT_EQ(read32le(buf), expect);
  tcc_free(s1->sym_attrs);
  return 0;
}

/* ------------------------------------------------------------------ */
/* relocate(): no-op / trivial-store families                          */
/* ------------------------------------------------------------------ */

UT_TEST(test_relocate_copy_is_noop)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0xCAFEBABEu);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_COPY);
  relocate(s1, &rel, R_ARM_COPY, buf, 0, 0x1234);

  UT_ASSERT_EQ(read32le(buf), 0xCAFEBABEu);
  return 0;
}

UT_TEST(test_relocate_none_is_noop)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0x11223344u);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_NONE);
  relocate(s1, &rel, R_ARM_NONE, buf, 0, 0);

  UT_ASSERT_EQ(read32le(buf), 0x11223344u);
  return 0;
}

UT_TEST(test_relocate_relative_is_noop_without_pe)
{
  /* TCC_TARGET_PE is not defined in this build (armv8m target), so
   * R_ARM_RELATIVE's body is entirely under #ifdef TCC_TARGET_PE and this
   * case is a pure no-op here. */
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0x55667788u);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_RELATIVE);
  relocate(s1, &rel, R_ARM_RELATIVE, buf, 0, 0x9999);

  UT_ASSERT_EQ(read32le(buf), 0x55667788u);
  return 0;
}

UT_TEST(test_relocate_glob_dat_and_jump_slot_store_val_directly)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0);
  ElfW_Rel rel = rel_for_sym(1, R_ARM_GLOB_DAT);
  relocate(s1, &rel, R_ARM_GLOB_DAT, buf, 0, 0xABCD1234u);
  UT_ASSERT_EQ(read32le(buf), 0xABCD1234u);

  write32le(buf, 0);
  rel = rel_for_sym(1, R_ARM_JUMP_SLOT);
  relocate(s1, &rel, R_ARM_JUMP_SLOT, buf, 0, 0x11112222u);
  UT_ASSERT_EQ(read32le(buf), 0x11112222u);
  return 0;
}

UT_TEST(test_relocate_v4bx_rewrites_bx_to_mov_pc)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  /* BX R0 in ARM encoding: cond=1110, 0x012FFF10 | Rm(0) */
  uint8_t buf[4];
  write32le(buf, 0xE12FFF10u);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_V4BX);
  relocate(s1, &rel, R_ARM_V4BX, buf, 0, 0);

  /* MOV PC, R0 == 0xE1A0F000 */
  UT_ASSERT_EQ(read32le(buf), 0xE1A0F000u);
  return 0;
}

UT_TEST(test_relocate_v4bx_leaves_non_bx_instruction_alone)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0xE3A00000u); /* MOV R0, #0 -- not a BX form */

  ElfW_Rel rel = rel_for_sym(1, R_ARM_V4BX);
  relocate(s1, &rel, R_ARM_V4BX, buf, 0, 0);

  UT_ASSERT_EQ(read32le(buf), 0xE3A00000u);
  return 0;
}

/* ------------------------------------------------------------------ */
/* relocate(): R_ARM_PREL31                                             */
/* ------------------------------------------------------------------ */

UT_TEST(test_relocate_prel31_adds_offset_preserves_top_bit)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);

  uint8_t buf[4];
  write32le(buf, 0x80000000u); /* top bit set, 31-bit field is 0 */

  addr_t addr = 0x1000;
  addr_t val = 0x1040;
  ElfW_Rel rel = rel_for_sym(1, R_ARM_PREL31);
  relocate(s1, &rel, R_ARM_PREL31, buf, addr, val);

  uint32_t result = read32le(buf);
  UT_ASSERT_EQ(result & 0x80000000u, 0x80000000u); /* top bit preserved */
  UT_ASSERT_EQ(result & 0x7fffffffu, 0x40u);        /* val - addr */
  return 0;
}

/* ------------------------------------------------------------------ */
/* relocate(): Thumb-2 branch-family encodings                          */
/* ------------------------------------------------------------------ */

static void ut_set_nonweak_sym(Elf32_Sym *syms, int idx)
{
  syms[idx].st_shndx = 1; /* defined (not SHN_UNDEF) */
  syms[idx].st_info = ELF32_ST_INFO(STB_GLOBAL, 0);
}

static void ut_set_weak_undef_sym(Elf32_Sym *syms, int idx)
{
  syms[idx].st_shndx = SHN_UNDEF;
  syms[idx].st_info = ELF32_ST_INFO(STB_WEAK, 0);
}

UT_TEST(test_relocate_thm_jump6_forward_branch)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_nonweak_sym(syms, 1);

  uint8_t buf[2];
  write16le(buf, 0xb100); /* CBZ-family opcode skeleton, i/imm5 bits zero */

  addr_t addr = 0x1000;
  addr_t val = 0x1000 + 4 + 8; /* x = (val-addr-4)>>1 = 4 */
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_JUMP6);
  relocate(s1, &rel, R_ARM_THM_JUMP6, buf, addr, val);

  uint16_t result = (uint16_t)(buf[0] | (buf[1] << 8));
  /* i = (4>>5)&1 = 0, imm5 = 4&0x1f = 4 -> imm5<<3 = 0x20 */
  UT_ASSERT_EQ(result, (uint16_t)(0xb100 | (4 << 3)));
  return 0;
}

UT_TEST(test_relocate_thm_jump6_negative_offset_forces_nop)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_nonweak_sym(syms, 1);

  uint8_t buf[2];
  write16le(buf, 0x1234);

  addr_t addr = 0x2000;
  addr_t val = 0x1000; /* val - addr - 4 < 0 */
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_JUMP6);
  relocate(s1, &rel, R_ARM_THM_JUMP6, buf, addr, val);

  uint16_t result = (uint16_t)(buf[0] | (buf[1] << 8));
  UT_ASSERT_EQ(result, 0xbf00); /* documented NOP fallback */
  return 0;
}

UT_TEST(test_relocate_thm_jump6_weak_undef_is_skipped)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_weak_undef_sym(syms, 1);

  uint8_t buf[2];
  write16le(buf, 0x4242);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_JUMP6);
  relocate(s1, &rel, R_ARM_THM_JUMP6, buf, 0x1000, 0x2000);

  uint16_t result = (uint16_t)(buf[0] | (buf[1] << 8));
  UT_ASSERT_EQ(result, 0x4242); /* untouched: weak undef reference bails out */
  return 0;
}

UT_TEST(test_relocate_thm_jump19_forward_branch)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_nonweak_sym(syms, 1);

  uint8_t buf[4];
  /* T3 conditional-branch skeleton with cond bits set, all offset bits 0. */
  write16le(buf, 0xf000);
  write16le(buf + 2, 0x8000);

  addr_t addr = 0x1000;
  addr_t val = 0x1000 + 0x100; /* x = val - addr = 0x100 */
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_JUMP19);
  relocate(s1, &rel, R_ARM_THM_JUMP19, buf, addr, val);

  uint16_t hi = (uint16_t)(buf[0] | (buf[1] << 8));
  uint16_t lo = (uint16_t)(buf[2] | (buf[3] << 8));
  /* Decode back per the same T3 formula the pass uses. */
  int s = (hi >> 10) & 1;
  int j1 = (lo >> 13) & 1;
  int j2 = (lo >> 11) & 1;
  int imm6 = hi & 0x3f;
  int imm11 = lo & 0x7ff;
  int decoded = (s << 20) | (j2 << 19) | (j1 << 18) | (imm6 << 12) | (imm11 << 1);
  UT_ASSERT_EQ(decoded, 0x100);
  return 0;
}

UT_TEST(test_relocate_thm_jump19_out_of_range_reports_error)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_nonweak_sym(syms, 1);

  uint8_t buf[4];
  write16le(buf, 0xf000);
  write16le(buf + 2, 0x8000);

  addr_t addr = 0;
  addr_t val = 0x200000; /* outside +-1MB */
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_JUMP19);
  relocate(s1, &rel, R_ARM_THM_JUMP19, buf, addr, val);

  UT_ASSERT(ut_arm_link_error_calls >= 1);
  return 0;
}

UT_TEST(test_relocate_thm_pc22_call_forward)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_nonweak_sym(syms, 1);
  /* No PLT configured -> to_plt stays 0; keep the offset comfortably small
   * so the "target must be a call or PLT jump" range guard is not hit. */

  uint8_t buf[4];
  write16le(buf, 0xf000);
  /* Only bits 15/14 (0xc000) survive the `lo & 0xd000` preserve-mask along
   * with bit 12; start bit 12 clear so the assertion below isolates
   * `blx_bit` rather than an incidentally-preserved input bit. */
  write16le(buf + 2, 0xc000); /* lo bits: j1/j2 will be recomputed */

  addr_t addr = 0x1000;
  addr_t val = 0x1000 + 0x200;
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_PC22);
  relocate(s1, &rel, R_ARM_THM_PC22, buf, addr, val);

  uint16_t lo = (uint16_t)(buf[2] | (buf[3] << 8));
  /* is_call => blx_bit forced to 0, i.e. bit 12 of lo must be clear. */
  UT_ASSERT_EQ(lo & (1 << 12), 0);
  UT_ASSERT_EQ(ut_arm_link_error_calls, 0);
  return 0;
}

UT_TEST(test_relocate_thm_jump24_sets_blx_bit)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_nonweak_sym(syms, 1);

  uint8_t buf[4];
  write16le(buf, 0xf000);
  write16le(buf + 2, 0xc000); /* bit 12 clear so blx_bit is what sets it */

  addr_t addr = 0x1000;
  addr_t val = 0x1000 + 0x200;
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_JUMP24);
  relocate(s1, &rel, R_ARM_THM_JUMP24, buf, addr, val);

  uint16_t lo = (uint16_t)(buf[2] | (buf[3] << 8));
  /* is_call is false for JUMP24 -> blx_bit (1<<12) stays set. */
  UT_ASSERT_EQ(lo & (1 << 12), (1 << 12));
  return 0;
}

UT_TEST(test_relocate_thm_pc22_weak_undef_is_skipped)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_weak_undef_sym(syms, 1);

  uint8_t buf[4];
  write16le(buf, 0x1111);
  write16le(buf + 2, 0x2222);

  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_PC22);
  relocate(s1, &rel, R_ARM_THM_PC22, buf, 0x1000, 0x2000);

  uint16_t hi = (uint16_t)(buf[0] | (buf[1] << 8));
  uint16_t lo = (uint16_t)(buf[2] | (buf[3] << 8));
  UT_ASSERT_EQ(hi, 0x1111);
  UT_ASSERT_EQ(lo, 0x2222);
  return 0;
}

UT_TEST(test_relocate_thm_pc12_forward)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_nonweak_sym(syms, 1);

  uint8_t buf[4];
  write16le(buf, 0x0000);
  write16le(buf + 2, 0x0000);

  addr_t addr = 0x1004; /* addr & -4 == 0x1004 */
  addr_t val = 0x1004 + 0x40; /* val > addr -> x = val - addr - 4 */
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_PC12);
  relocate(s1, &rel, R_ARM_THM_PC12, buf, addr, val);

  uint16_t lo = (uint16_t)(buf[2] | (buf[3] << 8));
  UT_ASSERT_EQ(lo & 0xfff, (0x40 - 4) & 0xfff);
  return 0;
}

UT_TEST(test_relocate_thm_pc8_backward_sets_subtract_bit)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_nonweak_sym(syms, 1);

  uint8_t buf[4];
  /* bit 7 of the first halfword (0x0080) is the "add/sub" bit the backward
   * path clears via `& 0xff7f`. */
  write16le(buf, 0x00c0);
  write16le(buf + 2, 0x0000);

  addr_t addr = 0x2004; /* addr & -4 == 0x2004 */
  addr_t val = 0x2004 - 0x40; /* val < addr -> backward branch */
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_PC8);
  relocate(s1, &rel, R_ARM_THM_PC8, buf, addr, val);

  uint16_t hi = (uint16_t)(buf[0] | (buf[1] << 8));
  UT_ASSERT_EQ(hi & 0x0080, 0); /* subtract bit cleared */
  uint16_t lo = (uint16_t)(buf[2] | (buf[3] << 8));
  /* x = (addr + 4 - val) >> 2 = (0x2008 - 0x1fc4) >> 2 */
  uint32_t expect = ((addr + 4 - val) >> 2) & 0xff;
  UT_ASSERT_EQ(lo & 0xff, expect);
  return 0;
}

UT_TEST(test_relocate_thm_alu_prel_11_0_forward)
{
  TCCState s1_storage;
  TCCState *s1 = &s1_storage;
  Section symtab_sec;
  Elf32_Sym syms[2];
  ut_arm_link_reset(s1, &symtab_sec, syms, 2);
  ut_set_nonweak_sym(syms, 1);

  uint8_t buf[4];
  write16le(buf, 0x0000);
  write16le(buf + 2, 0x0000);

  addr_t addr = 0x1004; /* addr & -4 == 0x1004 */
  addr_t val = 0x1004 + 0x80; /* val >= addr -> forward: x = val-(addr+4) */
  ElfW_Rel rel = rel_for_sym(1, R_ARM_THM_ALU_PREL_11_0);
  relocate(s1, &rel, R_ARM_THM_ALU_PREL_11_0, buf, addr, val);

  uint16_t hi = (uint16_t)(buf[0] | (buf[1] << 8));
  uint16_t lo = (uint16_t)(buf[2] | (buf[3] << 8));
  int i = (hi >> 10) & 1;
  int imm3 = (lo >> 12) & 0x7;
  int imm8 = lo & 0xff;
  int decoded = i << 11 | imm3 << 8 | imm8;
  /* x = val - (addr+4) = 0x80 - 4 = 0x7c, which is positive so encoded
   * directly (no negate). */
  UT_ASSERT_EQ(decoded, (int)(0x80 - 4));
  return 0;
}
