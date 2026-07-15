/*
 *  test_tccdbg.c - suite for tccdbg.c DWARF helper routines
 */

#define USING_GLOBALS
#include "tcc.h"
#include "ut.h"

/* Stubs for production symbols that some tccdbg.c code paths touch when
 * higher-level helpers are exercised.  The real definitions live in modules
 * (tccelf.c, tccpp.c, tccgen.c) that are not linked into the main unit-test
 * binary, so provide minimal stand-ins here for the functions under test. */
void utb_set_tok_str(int tok, const char *name);

ST_DATA int func_ind;

ST_FUNC void put_elf_reloca(Section *symtab, Section *s, unsigned long offset,
                            int type, int symbol, addr_t addend)
{
  (void)symtab;
  (void)s;
  (void)offset;
  (void)type;
  (void)symbol;
  (void)addend;
}

static void ut_cstr_realloc(CString *cstr, int new_size)
{
  if (new_size > cstr->size_allocated)
  {
    cstr->size_allocated = (new_size + 63) & ~63;
    cstr->data = (char *)tcc_realloc(cstr->data, cstr->size_allocated);
  }
}

ST_FUNC void cstr_new(CString *cstr)
{
  cstr->size = 0;
  cstr->size_allocated = 0;
  cstr->data = NULL;
}

ST_FUNC void cstr_free(CString *cstr)
{
  tcc_free(cstr->data);
  cstr->data = NULL;
  cstr->size = 0;
  cstr->size_allocated = 0;
}

ST_FUNC int cstr_printf(CString *cstr, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0)
    return -1;
  ut_cstr_realloc(cstr, cstr->size + n + 1);
  va_start(ap, fmt);
  vsnprintf(cstr->data + cstr->size, (size_t)n + 1, fmt, ap);
  va_end(ap);
  cstr->size += n;
  return n;
}

/*
 * Pull tccdbg.c into this test TU so its static helpers can be tested without
 * exporting them or linking the real public debug functions over existing UT
 * stubs used by other suites.
 */
#define tcc_debug_new ut_tccdbg_debug_new
#define tcc_eh_frame_start ut_tccdbg_eh_frame_start
#define tcc_eh_frame_end ut_tccdbg_eh_frame_end
#define tcc_eh_frame_hdr ut_tccdbg_eh_frame_hdr
#define tcc_debug_start ut_tccdbg_debug_start
#define tcc_debug_end ut_tccdbg_debug_end
#define tcc_debug_newfile ut_tccdbg_debug_newfile
#define tcc_debug_bincl ut_tccdbg_debug_bincl
#define tcc_debug_eincl ut_tccdbg_debug_eincl
#define tcc_debug_line ut_tccdbg_debug_line
#define tcc_debug_line_num ut_tccdbg_debug_line_num
#define tcc_debug_stabn ut_tccdbg_debug_stabn
#define tcc_debug_fix_anon ut_tccdbg_debug_fix_anon
#define tcc_add_debug_info ut_tccdbg_add_debug_info
#define tcc_debug_save_state ut_tccdbg_debug_save_state
#define tcc_debug_restore_state ut_tccdbg_debug_restore_state
#define tcc_debug_funcstart ut_tccdbg_debug_funcstart
#define tcc_debug_prolog_epilog ut_tccdbg_debug_prolog_epilog
#define tcc_debug_funcend ut_tccdbg_debug_funcend
#define tcc_debug_extern_sym ut_tccdbg_debug_extern_sym
#define tcc_debug_typedef ut_tccdbg_debug_typedef
#define tcc_tcov_block_begin ut_tccdbg_tcov_block_begin
#define tcc_tcov_block_end ut_tccdbg_tcov_block_end
#define tcc_tcov_check_line ut_tccdbg_tcov_check_line
#define tcc_tcov_start ut_tccdbg_tcov_start
#define tcc_tcov_end ut_tccdbg_tcov_end
#define tcc_tcov_reset_ind ut_tccdbg_tcov_reset_ind
void ut_tccdbg_debug_bincl(TCCState *s1);
void ut_tccdbg_debug_funcend(TCCState *s1, int size);

/* Stubs for tccdbg.c entry points that are not supplied by the main unit-test
 * binary's stub layer.  These are only used by the tccdbg.c TU included above. */

void put_extern_sym(Sym *sym, Section *section, addr_t value, unsigned long size)
{
  (void)sym;
  (void)section;
  (void)value;
  (void)size;
}

void gen_increment_tcov(SValue *sv)
{
  (void)sv;
}

/* Stubs for the ARM EH frame helpers referenced by tccdbg.c's
 * tcc_debug_frame_end when TCC_EH_FRAME is enabled. */
uint32_t pushed_registers;
int allocated_stack_size;

void *section_ptr_add(Section *sec, addr_t size);
#define dwarf_data1(s, data) (*(uint8_t *)section_ptr_add((s), 1) = (data))

int dwarf_arm_thumb_count_bits(uint32_t mask)
{
  int count = 0;
  while (mask)
  {
    count += mask & 1;
    mask >>= 1;
  }
  return count;
}

void dwarf_arm_thumb_emit_offsets(Section *sec, uint32_t mask)
{
  int reg;
  (void)sec;
  for (reg = 0; reg < 16; reg++)
    if (mask & (1u << reg))
      dwarf_data1(sec, DW_CFA_offset + reg);
}

void arm_ehabi_emit_function_entry(TCCState *s1)
{
  (void)s1;
}

char *pstrcat(char *buf, size_t buf_size, const char *s)
{
  size_t len = strlen(buf);
  size_t slen = strlen(s);
  size_t n = slen;
  if (len + n + 1 > buf_size)
    n = buf_size - len - 1;
  if (n > 0)
  {
    memmove(buf + len, s, n);
    buf[len + n] = 0;
  }
  return buf;
}

Section *new_section(TCCState *s1, const char *name, int sh_type, int sh_flags)
{
  Section *sec = (Section *)tcc_mallocz(sizeof(Section));
  sec->s1 = s1;
  sec->data = (unsigned char *)tcc_malloc(2048);
  sec->data_allocated = 2048;
  sec->sh_type = sh_type;
  sec->sh_flags = sh_flags;
  sec->sh_num = s1->nb_sections++;
  sec->sh_addralign = 1;
  (void)name;
  return sec;
}

void write64le(unsigned char *p, uint64_t x)
{
  p[0] = (unsigned char)(x & 0xff);
  p[1] = (unsigned char)((x >> 8) & 0xff);
  p[2] = (unsigned char)((x >> 16) & 0xff);
  p[3] = (unsigned char)((x >> 24) & 0xff);
  p[4] = (unsigned char)((x >> 32) & 0xff);
  p[5] = (unsigned char)((x >> 40) & 0xff);
  p[6] = (unsigned char)((x >> 48) & 0xff);
  p[7] = (unsigned char)((x >> 56) & 0xff);
}

uint64_t read64le(unsigned char *p)
{
  return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
         ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
         ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

#include "tccdbg.c"

void *section_ptr_add(Section *sec, addr_t size)
{
  unsigned long offset = sec->data_offset;
  if (!sec->data || offset + size > sec->data_allocated)
    return NULL;
  sec->data_offset = offset + size;
  return sec->data + offset;
}

static void test_section_reset(Section *sec, unsigned char *storage, size_t storage_size)
{
  memset(sec, 0, sizeof(*sec));
  sec->data = storage;
  sec->data_allocated = storage_size;
}

static int assert_bytes_eq(const unsigned char *actual, const unsigned char *expected, size_t n)
{
  for (size_t i = 0; i < n; i++)
    UT_ASSERT_EQ(actual[i], expected[i]);
  return 0;
}

/* Minimal TCCState + debug state for tests that need a dState. */
static TCCState *ut_dbg_make_state(void)
{
  TCCState *s1 = (TCCState *)tcc_mallocz(sizeof(*s1));
  s1->dState = (struct _tccdbg *)tcc_mallocz(sizeof(*s1->dState));
  return s1;
}

static void ut_dbg_free_state(TCCState *s1)
{
  int i;

  if (!s1)
    return;
  if (s1->dState)
  {
    /* dwarf_line macro is #undef'd at the end of tccdbg.c; access directly. */
    tcc_free(s1->dState->dwarf_line.line_data);
    s1->dState->dwarf_line.line_data = NULL;
    /* Free directory/file tables allocated by tcc_debug_start. */
    if (s1->dState->dwarf_line.dir_table)
    {
      for (i = 0; i < s1->dState->dwarf_line.dir_size; i++)
        tcc_free(s1->dState->dwarf_line.dir_table[i]);
      tcc_free(s1->dState->dwarf_line.dir_table);
      s1->dState->dwarf_line.dir_table = NULL;
      s1->dState->dwarf_line.dir_size = 0;
    }
    if (s1->dState->dwarf_line.filename_table)
    {
      for (i = 0; i < s1->dState->dwarf_line.filename_size; i++)
        tcc_free(s1->dState->dwarf_line.filename_table[i].name);
      tcc_free(s1->dState->dwarf_line.filename_table);
      s1->dState->dwarf_line.filename_table = NULL;
      s1->dState->dwarf_line.filename_size = 0;
    }
    /* These macros remain defined after the #include. */
    tcc_free(dwarf_text_sections);
    dwarf_text_sections = NULL;
    tcc_free(dwarf_line_relocs);
    dwarf_line_relocs = NULL;
    /* debug hash macros are #undef'd. */
    tcc_free(s1->dState->debug_hash);
    s1->dState->debug_hash = NULL;
    if (s1->dState->debug_anon_hash)
    {
      for (i = 0; i < s1->dState->n_debug_anon_hash; i++)
        tcc_free(s1->dState->debug_anon_hash[i].debug_type);
      tcc_free(s1->dState->debug_anon_hash);
      s1->dState->debug_anon_hash = NULL;
    }
    tcc_free(s1->dState);
  }
  tcc_free(s1);
}

static void ut_dbg_reset_tcov(TCCState *s1)
{
  if (tcov_section)
  {
    tcc_free(tcov_section->data);
    tcc_free(tcov_section);
  }
  tcov_section = NULL;
  if (s1->dState)
    memset(&s1->dState->tcov_data, 0, sizeof(s1->dState->tcov_data));
}

UT_TEST(test_dwarf_uleb128_size_boundaries)
{
  UT_ASSERT_EQ(dwarf_uleb128_size(0), 1);
  UT_ASSERT_EQ(dwarf_uleb128_size(1), 1);
  UT_ASSERT_EQ(dwarf_uleb128_size(0x7f), 1);
  UT_ASSERT_EQ(dwarf_uleb128_size(0x80), 2);
  UT_ASSERT_EQ(dwarf_uleb128_size(0x3fff), 2);
  UT_ASSERT_EQ(dwarf_uleb128_size(0x4000), 3);
  UT_ASSERT_EQ(dwarf_uleb128_size(624485), 3);
  UT_ASSERT_EQ(dwarf_uleb128_size(~0ULL), 10);
  return 0;
}

UT_TEST(test_dwarf_sleb128_size_boundaries)
{
  UT_ASSERT_EQ(dwarf_sleb128_size(0), 1);
  UT_ASSERT_EQ(dwarf_sleb128_size(1), 1);
  UT_ASSERT_EQ(dwarf_sleb128_size(63), 1);
  UT_ASSERT_EQ(dwarf_sleb128_size(64), 2);
  UT_ASSERT_EQ(dwarf_sleb128_size(-1), 1);
  UT_ASSERT_EQ(dwarf_sleb128_size(-64), 1);
  UT_ASSERT_EQ(dwarf_sleb128_size(-65), 2);
  UT_ASSERT_EQ(dwarf_sleb128_size(-624485), 3);
  return 0;
}

UT_TEST(test_dwarf_uleb128_encoding)
{
  Section sec;
  unsigned char data[16];
  const unsigned char encoded[] = {0xe5, 0x8e, 0x26};

  test_section_reset(&sec, data, sizeof(data));
  dwarf_uleb128(&sec, 624485);

  UT_ASSERT_EQ(sec.data_offset, sizeof(encoded));
  UT_ASSERT_EQ(assert_bytes_eq(sec.data, encoded, sizeof(encoded)), 0);
  return 0;
}

UT_TEST(test_dwarf_sleb128_encoding)
{
  Section sec;
  unsigned char data[16];
  const unsigned char encoded[] = {0x9b, 0xf1, 0x59};

  test_section_reset(&sec, data, sizeof(data));
  dwarf_sleb128(&sec, -624485);

  UT_ASSERT_EQ(sec.data_offset, sizeof(encoded));
  UT_ASSERT_EQ(assert_bytes_eq(sec.data, encoded, sizeof(encoded)), 0);
  return 0;
}

UT_TEST(test_dwarf_emit_reg_op_uses_short_form_for_reg0_to_reg31)
{
  Section sec;
  unsigned char data[16];

  test_section_reset(&sec, data, sizeof(data));
  dwarf_emit_reg_op(&sec, 0);
  dwarf_emit_reg_op(&sec, 31);

  UT_ASSERT_EQ(sec.data_offset, 2);
  UT_ASSERT_EQ(sec.data[0], DW_OP_reg0);
  UT_ASSERT_EQ(sec.data[1], DW_OP_reg0 + 31);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(31), 1);
  return 0;
}

UT_TEST(test_dwarf_emit_reg_op_uses_regx_for_large_registers)
{
  Section sec;
  unsigned char data[16];
  const unsigned char encoded[] = {DW_OP_regx, 0x20, DW_OP_regx, 0x80, 0x01};

  test_section_reset(&sec, data, sizeof(data));
  dwarf_emit_reg_op(&sec, 32);
  dwarf_emit_reg_op(&sec, 128);

  UT_ASSERT_EQ(sec.data_offset, sizeof(encoded));
  UT_ASSERT_EQ(assert_bytes_eq(sec.data, encoded, sizeof(encoded)), 0);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(32), 2);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(128), 3);
  return 0;
}

UT_TEST(test_dwarf_reg_piece_size_for_sym)
{
  struct debug_sym sym;
  memset(&sym, 0, sizeof(sym));

  UT_ASSERT_EQ(dwarf_reg_piece_size_for_sym(NULL), PTR_SIZE >= 8 ? 8 : 4);
  UT_ASSERT_EQ(dwarf_reg_piece_size_for_sym(&sym), PTR_SIZE >= 8 ? 8 : 4);

  sym.size = 1;
  UT_ASSERT_EQ(dwarf_reg_piece_size_for_sym(&sym), PTR_SIZE >= 8 ? 8 : 4);
  sym.size = 8;
  UT_ASSERT_EQ(dwarf_reg_piece_size_for_sym(&sym), 4);
  sym.size = 16;
  UT_ASSERT_EQ(dwarf_reg_piece_size_for_sym(&sym), 8);
  return 0;
}

UT_TEST(test_dwarf_emit_regpair_expr)
{
  Section sec;
  unsigned char data[16];
  const unsigned char encoded[] = {DW_OP_reg0 + 1, DW_OP_piece, 4, DW_OP_regx, 0x20, DW_OP_piece, 4};

  test_section_reset(&sec, data, sizeof(data));
  dwarf_emit_regpair_expr(&sec, 1, 32, 4);

  UT_ASSERT_EQ(sec.data_offset, sizeof(encoded));
  UT_ASSERT_EQ(assert_bytes_eq(sec.data, encoded, sizeof(encoded)), 0);
  UT_ASSERT_EQ(dwarf_loc_regpair_len(1, 32, 4), sizeof(encoded));
  return 0;
}

UT_TEST(test_dwarf_line_op_grows_buffer)
{
  TCCState *s1 = ut_dbg_make_state();
  int i;

  dwarf_line_op(s1, 0xAB);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_size, 1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[0], 0xAB);

  for (i = 0; i < 1024; i++)
    dwarf_line_op(s1, (unsigned char)i);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_size, 1025);
  UT_ASSERT(s1->dState->dwarf_line.line_max_size >= 1025);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_uleb128_op_encoding)
{
  TCCState *s1 = ut_dbg_make_state();
  const unsigned char encoded[] = {0xe5, 0x8e, 0x26};

  dwarf_uleb128_op(s1, 624485);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_size, sizeof(encoded));
  UT_ASSERT_EQ(assert_bytes_eq(s1->dState->dwarf_line.line_data, encoded, sizeof(encoded)), 0);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_sleb128_op_encoding)
{
  TCCState *s1 = ut_dbg_make_state();
  const unsigned char encoded[] = {0x9b, 0xf1, 0x59};

  dwarf_sleb128_op(s1, -624485);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_size, sizeof(encoded));
  UT_ASSERT_EQ(assert_bytes_eq(s1->dState->dwarf_line.line_data, encoded, sizeof(encoded)), 0);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_get_section_sym_returns_symbol_index)
{
  TCCState *s1 = ut_dbg_make_state();
  Section sec;
  unsigned char data[16];

  test_section_reset(&sec, data, sizeof(data));
  sec.s1 = s1;
  sec.sh_num = 5;
  symtab_section = (Section *)1; /* non-NULL, treated as opaque by stub */

  int sym = dwarf_get_section_sym(&sec);
  UT_ASSERT_EQ(sym, 6); /* stub returns shndx + 1 */

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_register_text_section_tracks_unique_sections)
{
  TCCState *s1 = ut_dbg_make_state();
  Section sec1, sec2;
  unsigned char data1[16], data2[16];

  symtab_section = (Section *)1;

  test_section_reset(&sec1, data1, sizeof(data1));
  sec1.sh_num = 1;
  test_section_reset(&sec2, data2, sizeof(data2));
  sec2.sh_num = 2;

  int sym1 = dwarf_register_text_section(s1, &sec1);
  int sym1_again = dwarf_register_text_section(s1, &sec1);
  int sym2 = dwarf_register_text_section(s1, &sec2);

  UT_ASSERT_EQ(sym1, sym1_again);
  UT_ASSERT_EQ(n_dwarf_text_sections, 2);
  UT_ASSERT_EQ(dwarf_text_sections[0].section, &sec1);
  UT_ASSERT_EQ(dwarf_text_sections[1].section, &sec2);
  (void)sym2;

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_add_line_reloc_grows_array)
{
  TCCState *s1 = ut_dbg_make_state();
  int i;

  for (i = 0; i < 20; i++)
    dwarf_add_line_reloc(s1, i * 4, i + 1, i);

  UT_ASSERT_EQ(n_dwarf_line_relocs, 20);
  for (i = 0; i < 20; i++)
  {
    UT_ASSERT_EQ(dwarf_line_relocs[i].line_data_offset, i * 4);
    UT_ASSERT_EQ(dwarf_line_relocs[i].sym_index, i + 1);
    UT_ASSERT_EQ(dwarf_line_relocs[i].addend, i);
  }

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_find_add_remove)
{
  TCCState *s1 = ut_dbg_make_state();
  Sym sym = {0};

  UT_ASSERT_EQ(tcc_debug_find(s1, &sym, 0), -1);
  int type = tcc_debug_add(s1, &sym, 0);
  UT_ASSERT_EQ(tcc_debug_find(s1, &sym, 0), type);
  UT_ASSERT_EQ(type, 1);

  tcc_debug_remove(s1, &sym);
  UT_ASSERT_EQ(tcc_debug_find(s1, &sym, 0), -1);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_check_anon_records_offsets)
{
  TCCState *s1 = ut_dbg_make_state();
  Sym outer = {0};
  Sym inner = {0};

  outer.type.t = VT_STRUCT;
  outer.type.ref = &inner;
  inner.type.t = VT_STRUCT;
  inner.c = -1;

  /* tcc_debug_check_anon records offsets only for anon entries that already
   * exist (created by tcc_debug_find). */
  tcc_debug_find(s1, &inner, 1);
  UT_ASSERT_EQ(s1->dState->n_debug_anon_hash, 1);

  tcc_debug_check_anon(s1, &outer, 42);
  UT_ASSERT_EQ(s1->dState->debug_anon_hash[0].type, &inner);
  UT_ASSERT_EQ(s1->dState->debug_anon_hash[0].n_debug_type, 1);
  UT_ASSERT_EQ(s1->dState->debug_anon_hash[0].debug_type[0], 42);

  tcc_debug_check_anon(s1, &outer, 99);
  UT_ASSERT_EQ(s1->dState->debug_anon_hash[0].n_debug_type, 2);
  UT_ASSERT_EQ(s1->dState->debug_anon_hash[0].debug_type[1], 99);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_stabs_accumulates_symbols)
{
  TCCState *s1 = ut_dbg_make_state();
  struct _debug_info info = {0};
  BufferedFile bf = {0};

  file = &bf;
  s1->dState->debug_info = &info;
  tcc_debug_stabs(s1, "local_var", N_LSYM, 0x123, NULL, 0, 0, 5, 4);

  UT_ASSERT_EQ(info.n_sym, 1);
  UT_ASSERT_STREQ(info.sym[0].str, "local_var");
  UT_ASSERT_EQ(info.sym[0].type, N_LSYM);
  UT_ASSERT_EQ(info.sym[0].value, 0x123);
  UT_ASSERT_EQ(info.sym[0].vreg, 5);
  UT_ASSERT_EQ(info.sym[0].size, 4);

  tcc_debug_stabs(s1, "second", N_LSYM, 0, NULL, 0, 0, -1, 0);
  UT_ASSERT_EQ(info.n_sym, 2);

  tcc_free(info.sym[0].str);
  tcc_free(info.sym[1].str);
  tcc_free(info.sym);

  s1->dState->debug_info = NULL;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_stabs_without_debug_info_calls_put_stabs)
{
  TCCState *s1 = ut_dbg_make_state();

  /* Should not crash; put_stabs/put_stabs_r are no-ops in tccdbg.c. */
  tcc_debug_stabs(s1, "x", N_LSYM, 0x42, NULL, 0, 0, -1, 0);
  tcc_debug_stabs(s1, "y", N_LSYM, 0, (Section *)1, 1, 0, -1, 0);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_stabn_builds_scope_tree)
{
  TCCState *s1 = ut_dbg_make_state();

  s1->do_debug = 1;
  tcc_debug_stabn(s1, N_LBRAC, 10);
  UT_ASSERT(s1->dState->debug_info != NULL);
  UT_ASSERT_EQ(s1->dState->debug_info->start, 10);

  tcc_debug_stabn(s1, N_RBRAC, 20);
  UT_ASSERT_EQ(s1->dState->debug_info, NULL);
  UT_ASSERT(s1->dState->debug_info_root != NULL);
  UT_ASSERT_EQ(s1->dState->debug_info_root->end, 20);

  tcc_free(s1->dState->debug_info_root);
  s1->dState->debug_info_root = NULL;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_save_restore_state)
{
  TCCState *s1 = ut_dbg_make_state();
  struct _debug_info info = {0};
  void *saved_info, *saved_root;

  s1->dState->debug_info = &info;
  s1->dState->debug_info_root = &info;

  tcc_debug_save_state(s1, &saved_info, &saved_root);
  UT_ASSERT_EQ(saved_info, &info);
  UT_ASSERT_EQ(saved_root, &info);

  s1->dState->debug_info = NULL;
  s1->dState->debug_info_root = NULL;
  tcc_debug_restore_state(s1, saved_info, saved_root);
  UT_ASSERT_EQ(s1->dState->debug_info, &info);
  UT_ASSERT_EQ(s1->dState->debug_info_root, &info);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_loc_reg_op_len_edge_cases)
{
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(-1), 0); /* invalid regno: no bytes */
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(0), 1);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(31), 1);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(32), 2);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(16383), 3);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(16384), 4);
  return 0;
}

UT_TEST(test_dwarf_emit_reg_op_negative_reg_emits_nothing)
{
  Section sec;
  unsigned char data[32];

  test_section_reset(&sec, data, sizeof(data));
  dwarf_emit_reg_op(&sec, -1);
  UT_ASSERT_EQ(sec.data_offset, 0); /* invalid regno: emit nothing */

  return 0;
}

UT_TEST(test_dwarf_file_tracks_paths)
{
  TCCState *s1 = ut_dbg_make_state();
  BufferedFile bf = {0};
  char name1[] = "/a/b.c";
  char name2[] = "/a/c.c";
  char name3[] = "d.c";
  char cmd[] = "<command line>";

  file = &bf;
  s1->dwarf = 4;

  strcpy(bf.filename, name1);
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_size, 1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.dir_size, 1);
  UT_ASSERT_STREQ(s1->dState->dwarf_line.filename_table[0].name, "b.c");
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_table[0].dir_entry, 1);

  /* Cached lookup for the same path.
   * Note: dwarf_file assumes a prior tcc_debug_start-style initialization
   * that reserves entry 0 for the compilation-unit filename.  Our minimal
   * state starts with filename_size == 0, so the cache lookup at index 1
   * misses and a second entry is created.  The assertions below document
   * this out-of-context behavior rather than the production-init path. */
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 2);
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_size, 2);

  /* Same directory, new basename. */
  strcpy(bf.filename, name2);
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 3);
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_size, 3);

  /* Baseline file with no directory. */
  strcpy(bf.filename, name3);
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 4);
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_table[3].dir_entry, 0);

  /* Special <command line> short-circuit. */
  strcpy(bf.filename, cmd);
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 1);

  file = NULL;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_file_dwarf5_index_offset)
{
  TCCState *s1 = ut_dbg_make_state();
  BufferedFile bf = {0};
  char name[] = "x.c";

  file = &bf;
  s1->dwarf = 5;
  strcpy(bf.filename, name);
  dwarf_file(s1);

  /* With no prior entries and index_offset == 0, the first entry is 0. */
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 0);
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_size, 1);

  file = NULL;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_strp_appends_string_with_relocation_skipped)
{
  TCCState *s1 = ut_dbg_make_state();
  Section sec, str_sec, symtab;
  unsigned char sec_data[16], str_data[32], symtab_data[16];
  TCCState *old_tcc = tcc_state;

  test_section_reset(&sec, sec_data, sizeof(sec_data));
  test_section_reset(&str_sec, str_data, sizeof(str_data));
  test_section_reset(&symtab, symtab_data, sizeof(symtab_data));
  sec.s1 = s1;
  tcc_state = s1;
  dwarf_str_section = &str_sec;
  /* data_offset == 0 makes put_elf_reloca skip the relocation. */
  symtab_section = &symtab;
  s1->dState->dwarf_sym.str = 0;

  dwarf_strp(&sec, "hello");

  UT_ASSERT_EQ(str_sec.data_offset, 6);
  UT_ASSERT_STREQ((char *)str_data, "hello");
  UT_ASSERT_EQ(sec.data_offset, 4);
  UT_ASSERT_EQ(read32le(sec_data), 0);

  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_line_strp_appends_string)
{
  TCCState *s1 = ut_dbg_make_state();
  Section sec, str_sec, symtab;
  unsigned char sec_data[16], str_data[32], symtab_data[16];
  TCCState *old_tcc = tcc_state;

  test_section_reset(&sec, sec_data, sizeof(sec_data));
  test_section_reset(&str_sec, str_data, sizeof(str_data));
  test_section_reset(&symtab, symtab_data, sizeof(symtab_data));
  sec.s1 = s1;
  tcc_state = s1;
  dwarf_line_str_section = &str_sec;
  symtab_section = &symtab;
  s1->dState->dwarf_sym.line_str = 0;

  dwarf_line_strp(&sec, "world");

  UT_ASSERT_STREQ((char *)str_data, "world");
  UT_ASSERT_EQ(sec.data_offset, 4);

  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_reloc_skips_invalid_symbol_index)
{
  TCCState *s1 = ut_dbg_make_state();
  Section sec, symtab;
  unsigned char sec_data[16], symtab_data[16];
  TCCState *old_tcc = tcc_state;

  test_section_reset(&sec, sec_data, sizeof(sec_data));
  test_section_reset(&symtab, symtab_data, sizeof(symtab_data));
  sec.s1 = s1;
  tcc_state = s1;
  symtab_section = &symtab;

  dwarf_reloc(&sec, 0, R_DATA_32DW);

  UT_ASSERT_EQ(sec.data_offset, 0);
  UT_ASSERT_EQ(sec.reloc, NULL);

  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_put_stabs_variants_are_no_ops)
{
  TCCState *s1 = ut_dbg_make_state();

  put_stabs(s1, "x", N_LSYM, 0, 0, 0);
  put_stabs_r(s1, "x", N_LSYM, 0, 0, 0, (Section *)1, 1);
  put_stabn(s1, N_LSYM, 0, 0, 0);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_emit_set_address_encodes_extended_opcode)
{
  TCCState *s1 = ut_dbg_make_state();
  Section text;
  unsigned char text_data[16];
  unsigned char line_data[32];

  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_num = 3;
  text.sh_flags = SHF_EXECINSTR;

  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(sizeof(line_data));
  s1->dState->dwarf_line.line_max_size = sizeof(line_data);
  s1->dState->dwarf_line.line_size = 0;
  s1->dState->dwarf_line.cur_section = NULL;

  dwarf_emit_set_address(s1, &text, 0x1234);

  UT_ASSERT_EQ(s1->dState->dwarf_line.line_size, 1 + 1 + 1 + PTR_SIZE);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[0], 0);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[1], 1 + PTR_SIZE);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[2], DW_LNE_set_address);
  /* ARM uses REL, so the addend is stored in the section data. */
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[3], 0x34);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[4], 0x12);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_section, &text);
  UT_ASSERT_EQ(n_dwarf_line_relocs, 1);
  UT_ASSERT_EQ(dwarf_line_relocs[0].line_data_offset, 3);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_line_emits_dwarf_special_opcode)
{
  TCCState *s1 = ut_dbg_make_state();
  Section text;
  unsigned char text_data[16];
  unsigned char line_data[64];
  BufferedFile bf = {0};
  TCCState *old_tcc = tcc_state;
  int idx;

  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_flags = SHF_EXECINSTR;
  tcc_state = s1;
  cur_text_section = &text;
  s1->dwarf = 4;
  s1->do_debug = 1;
  s1->ir = 0;

  file = &bf;
  strcpy(bf.filename, "test.c");
  bf.line_num = 9;
  ind = 2;
  func_ind = -1;
  nocode_wanted = 0;
  s1->dState->new_file = 1;
  s1->dState->last_line_num = 1;

  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(sizeof(line_data));
  s1->dState->dwarf_line.line_max_size = sizeof(line_data);
  s1->dState->dwarf_line.line_size = 0;
  /* Pretend the current section/file/address were already established by a
   * prior tcc_debug_line call, so this invocation only emits the set_file
   * and special-opcode updates. */
  s1->dState->dwarf_line.cur_section = &text;
  s1->dState->dwarf_line.last_file = 0;
  s1->dState->dwarf_line.last_line = 1;
  s1->dState->dwarf_line.last_pc = 0;

  tcc_debug_line(s1);

  UT_ASSERT(s1->dState->dwarf_line.line_size > 0);
  idx = 0;
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[idx], DW_LNS_set_file);
  /* cur_file == 1, so the uleb128 size is one byte. */
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[idx + 1], 1);
  /* Special opcode: len_pc=1, len_line=8 -> 1*14 + 8 + 13 - (-5) == 40. */
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[idx + 2], 40);

  tcc_state = old_tcc;
  file = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_line_skips_non_executable_section)
{
  TCCState *s1 = ut_dbg_make_state();
  Section text;
  unsigned char text_data[16];
  unsigned char line_data[16];
  BufferedFile bf = {0};
  TCCState *old_tcc = tcc_state;

  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_flags = 0;
  tcc_state = s1;
  cur_text_section = &text;
  s1->dwarf = 4;
  s1->do_debug = 1;

  file = &bf;
  strcpy(bf.filename, "t.c");
  bf.line_num = 5;
  ind = 2;
  s1->dState->last_line_num = 0;

  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(sizeof(line_data));
  s1->dState->dwarf_line.line_max_size = sizeof(line_data);
  s1->dState->dwarf_line.line_size = 0;
  s1->dState->dwarf_line.cur_section = NULL;

  tcc_debug_line(s1);

  UT_ASSERT_EQ(s1->dState->dwarf_line.line_size, 0);

  tcc_state = old_tcc;
  file = NULL;
  ind = 0;
  s1->dState->last_line_num = 0;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_line_num_emits_and_dedupes)
{
  TCCState *s1 = ut_dbg_make_state();
  Section text;
  unsigned char text_data[16];
  unsigned char line_data[64];
  BufferedFile bf = {0};
  TCCState *old_tcc = tcc_state;
  int size_after_first;

  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_flags = SHF_EXECINSTR;
  tcc_state = s1;
  cur_text_section = &text;
  s1->dwarf = 4;
  s1->do_debug = 1;
  s1->ir = 0;

  file = &bf;
  strcpy(bf.filename, "t.c");
  bf.line_num = 1;
  ind = 2;
  func_ind = -1;
  nocode_wanted = 0;
  s1->dState->last_line_num = 0;

  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(sizeof(line_data));
  s1->dState->dwarf_line.line_max_size = sizeof(line_data);
  s1->dState->dwarf_line.line_size = 0;
  s1->dState->dwarf_line.cur_section = NULL;
  s1->dState->dwarf_line.last_file = 0;
  s1->dState->dwarf_line.last_line = 0;
  s1->dState->dwarf_line.last_pc = 0;

  tcc_debug_line_num(s1, 0);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_size, 0);

  tcc_debug_line_num(s1, 5);
  UT_ASSERT(s1->dState->dwarf_line.line_size > 0);
  size_after_first = s1->dState->dwarf_line.line_size;

  tcc_debug_line_num(s1, 5);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_size, size_after_first);

  tcc_state = old_tcc;
  file = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_newfile_bincl_eincl)
{
  TCCState *s1 = ut_dbg_make_state();
  BufferedFile bf = {0};
  char name[] = "inc.h";

  file = &bf;
  strcpy(bf.filename, name);
  bf.line_num = 1;
  s1->do_debug = 1;
  s1->dwarf = 4;
  s1->dState->new_file = 0;

  tcc_debug_newfile(s1);
  UT_ASSERT_EQ(s1->dState->new_file, 1);

  tcc_debug_bincl(s1);
  UT_ASSERT_EQ(s1->dState->new_file, 1);

  tcc_debug_eincl(s1);
  UT_ASSERT_EQ(s1->dState->new_file, 1);

  /* Non-DWARF path uses the no-op stab helpers. */
  s1->dwarf = 0;
  tcc_debug_newfile(s1);
  tcc_debug_bincl(s1);
  tcc_debug_eincl(s1);

  file = NULL;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_prolog_epilog_emits_flags)
{
  TCCState *s1 = ut_dbg_make_state();
  unsigned char line_data[32];

  s1->do_debug = 1;
  s1->dwarf = 4;
  ind = 4;
  s1->dState->dwarf_line.last_pc = 0;
  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(sizeof(line_data));
  s1->dState->dwarf_line.line_max_size = sizeof(line_data);
  s1->dState->dwarf_line.line_size = 0;

  tcc_debug_prolog_epilog(s1, 0);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[0], DW_LNS_set_prologue_end);
  /* advance_pc (1 byte) + uleb128(2) + copy (1 byte) after the flag. */
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_size, 4);

  s1->dState->dwarf_line.line_size = 0;
  tcc_debug_prolog_epilog(s1, 1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[0], DW_LNS_set_epilogue_begin);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_size, 1);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_find_returns_zero_for_existing_anon)
{
  TCCState *s1 = ut_dbg_make_state();
  Sym anon = {0};

  anon.type.t = VT_STRUCT;
  anon.c = -1;

  UT_ASSERT_EQ(tcc_debug_find(s1, &anon, 1), 0);
  UT_ASSERT_EQ(tcc_debug_find(s1, &anon, 1), 0);
  UT_ASSERT_EQ(s1->dState->n_debug_anon_hash, 1);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_remove_shifts_remaining_entries)
{
  TCCState *s1 = ut_dbg_make_state();
  Sym a = {0}, b = {0}, c = {0};

  (void)tcc_debug_add(s1, &a, 0);
  (void)tcc_debug_add(s1, &b, 0);
  (void)tcc_debug_add(s1, &c, 0);
  UT_ASSERT_EQ(s1->dState->n_debug_hash, 3);

  tcc_debug_remove(s1, &b);
  UT_ASSERT_EQ(s1->dState->n_debug_hash, 2);
  UT_ASSERT_EQ(s1->dState->debug_hash[0].type, &a);
  UT_ASSERT_EQ(s1->dState->debug_hash[1].type, &c);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_funcstart_and_funcend_dwarf)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Section info_sec, str_sec, text_sec, symtab;
  unsigned char info_data[1024];
  unsigned char str_data[256];
  unsigned char text_data[16];
  unsigned char symtab_data[16];
  Sym func_sym = {0};
  Sym ret_sym = {0};
  BufferedFile bf = {0};

  test_section_reset(&info_sec, info_data, sizeof(info_data));
  test_section_reset(&str_sec, str_data, sizeof(str_data));
  test_section_reset(&text_sec, text_data, sizeof(text_data));
  test_section_reset(&symtab, symtab_data, sizeof(symtab_data));
  info_sec.s1 = s1;
  str_sec.s1 = s1;
  text_sec.s1 = s1;
  text_sec.sh_flags = SHF_EXECINSTR;
  text_sec.sh_num = 1;

  tcc_state = s1;
  dwarf_info_section = &info_sec;
  dwarf_str_section = &str_sec;
  cur_text_section = &text_sec;
  text_section = &text_sec;
  symtab_section = &symtab;

  s1->do_debug = 1;
  s1->dwarf = 4;
  s1->ir = 0;
  s1->do_backtrace = 0;

  funcname = "test_fn";
  file = &bf;
  strcpy(bf.filename, "test.c");
  bf.line_num = 1;

  ind = 2;
  func_ind = 0;
  s1->dState->last_line_num = 0;

  s1->dState->dwarf_info.start = 0;
  s1->dState->dwarf_sym.str = 0;
  s1->dState->dwarf_sym.line_str = 0;
  s1->dState->dwarf_line.cur_section = NULL;
  s1->dState->dwarf_line.last_file = 0;
  s1->dState->dwarf_line.last_line = 0;
  s1->dState->dwarf_line.last_pc = 0;
  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(256);
  s1->dState->dwarf_line.line_max_size = 256;
  s1->dState->dwarf_line.line_size = 0;

  func_sym.type.t = VT_FUNC;
  func_sym.type.ref = &ret_sym;
  ret_sym.type.t = VT_INT;

  tcc_debug_funcstart(s1, &func_sym);
  UT_ASSERT(s1->dState->debug_info != NULL);
  UT_ASSERT(s1->dState->debug_info_root != NULL);

  tcc_debug_funcend(s1, 10);
  UT_ASSERT(s1->dState->debug_info_root == NULL);
  UT_ASSERT(info_sec.data_offset > 0);

  tcc_state = old_tcc;
  file = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_tcov_start_creates_section_when_enabled)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;

  tcc_state = s1;
  ut_dbg_reset_tcov(s1);

  s1->test_coverage = 0;
  tcc_tcov_start(s1);
  UT_ASSERT_EQ(tcov_section, (Section *)NULL);

  s1->test_coverage = 1;
  tcc_tcov_start(s1);
  UT_ASSERT(tcov_section != NULL);
  UT_ASSERT_EQ(tcov_section->data_offset, 4);
  UT_ASSERT_EQ(s1->dState->tcov_data.offset, 0UL);
  UT_ASSERT_EQ(s1->dState->tcov_data.ind, 0);
  UT_ASSERT_EQ(s1->dState->tcov_data.line, 0);

  ut_dbg_reset_tcov(s1);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_tcov_end_appends_terminators)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  unsigned char *data;

  tcc_state = s1;
  ut_dbg_reset_tcov(s1);
  s1->test_coverage = 1;
  tcc_tcov_start(s1);
  data = tcov_section->data;

  tcov_section->data_offset = 4;
  strcpy((char *)(data + 4), "file.c");
  tcov_section->data_offset += strlen("file.c") + 1;
  s1->dState->tcov_data.last_file_name = 4;
  s1->dState->tcov_data.last_func_name = 0;

  size_t before = tcov_section->data_offset;
  tcc_tcov_end(s1);
  /* tcc_tcov_end appends one NUL terminator byte for the active file name. */
  UT_ASSERT_EQ(tcov_section->data_offset, before + 1);
  UT_ASSERT_EQ(((unsigned char *)tcov_section->data)[before], 0);

  ut_dbg_reset_tcov(s1);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_tcov_reset_ind_zeroes_ind)
{
  TCCState *s1 = ut_dbg_make_state();
  s1->dState->tcov_data.ind = 42;
  tcc_tcov_reset_ind(s1);
  UT_ASSERT_EQ(s1->dState->tcov_data.ind, 0);
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_tcov_block_end_writes_line_range)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  unsigned char *data;

  tcc_state = s1;
  ut_dbg_reset_tcov(s1);
  s1->test_coverage = 1;
  tcc_tcov_start(s1);

  data = tcov_section->data;
  tcov_section->data_offset = 4;
  s1->dState->tcov_data.offset = 4;
  write64le(data + 4, (5ULL << 8) | 0xff);

  tcc_tcov_block_end(s1, 10);
  UT_ASSERT_EQ(s1->dState->tcov_data.offset, 0UL);
  UT_ASSERT_EQ(read64le(data + 4) >> 36, 10ULL);

  ut_dbg_reset_tcov(s1);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_tcov_check_line_ends_block_on_gap)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};

  tcc_state = s1;
  ut_dbg_reset_tcov(s1);
  s1->test_coverage = 1;
  tcc_tcov_start(s1);
  file = &bf;
  bf.true_filename = "t.c";
  bf.line_num = 5;
  s1->dState->tcov_data.line = 3;

  tcc_tcov_check_line(s1, 0);
  UT_ASSERT_EQ(s1->dState->tcov_data.offset, 0UL);

  file = NULL;
  ut_dbg_reset_tcov(s1);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_tcov_block_begin_records_file_and_function)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};

  tcc_state = s1;
  ut_dbg_reset_tcov(s1);
  s1->test_coverage = 1;
  tcc_tcov_start(s1);

  file = &bf;
  bf.true_filename = "file.c";
  funcname = "my_func";
  ind = 10;
  s1->dState->tcov_data.ind = 10;
  s1->dState->tcov_data.line = 5;
  bf.line_num = 5;

  tcc_tcov_block_begin(s1);

  UT_ASSERT(s1->dState->tcov_data.last_file_name != 0);
  UT_ASSERT_STREQ((char *)tcov_section->data + s1->dState->tcov_data.last_file_name, "file.c");
  UT_ASSERT(s1->dState->tcov_data.last_func_name != 0);
  UT_ASSERT_STREQ((char *)tcov_section->data + s1->dState->tcov_data.last_func_name, "my_func");

  file = NULL;
  funcname = NULL;
  ind = 0;
  ut_dbg_reset_tcov(s1);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_new_initializes_state_and_sections)
{
  TCCState *s1 = ut_dbg_make_state();
  int nb_before;

  tcc_free(s1->dState);
  s1->dState = NULL;
  s1->dwarf = 0;
  s1->do_debug = 1;
  s1->nb_sections = 0;
  nb_before = s1->nb_sections;

  tcc_debug_new(s1);

  UT_ASSERT(s1->dState != NULL);
  UT_ASSERT_EQ(s1->dwarf, DEFAULT_DWARF_VERSION);
  UT_ASSERT_EQ(s1->dwlo, nb_before);
  UT_ASSERT(s1->dwhi > s1->dwlo);
  UT_ASSERT(dwarf_info_section != NULL);
  UT_ASSERT(dwarf_abbrev_section != NULL);
  UT_ASSERT(dwarf_line_section != NULL);
  UT_ASSERT(dwarf_aranges_section != NULL);
  UT_ASSERT(dwarf_ranges_section != NULL);
  UT_ASSERT(dwarf_str_section != NULL);
  UT_ASSERT(dwarf_line_str_section != NULL);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_new_preserves_existing_dstate)
{
  TCCState *s1 = ut_dbg_make_state();
  struct _tccdbg *existing = s1->dState;

  s1->dwarf = 0;
  s1->do_debug = 1;
  s1->nb_sections = 0;
  tcc_debug_new(s1);
  UT_ASSERT_EQ(s1->dState, existing);

  ut_dbg_free_state(s1);
  return 0;
}

/* Wrapper around tcc_debug_end that nulls the fields the production function
 * frees but does not zero, so the subsequent ut_dbg_free_state does not
 * double-free. */
static void ut_dbg_debug_end(TCCState *s1)
{
  tcc_debug_end(s1);
  if (s1->dState)
  {
    s1->dState->dwarf_line.line_data = NULL;
    s1->dState->dwarf_line.line_size = 0;
    s1->dState->dwarf_line.line_max_size = 0;
    s1->dState->dwarf_line.dir_table = NULL;
    s1->dState->dwarf_line.dir_size = 0;
    s1->dState->dwarf_line.filename_table = NULL;
    s1->dState->dwarf_line.filename_size = 0;
    s1->dState->debug_hash = NULL;
    s1->dState->n_debug_hash = 0;
    s1->dState->debug_anon_hash = NULL;
    s1->dState->n_debug_anon_hash = 0;
  }
}

/* Minimal setup for tests that exercise tcc_debug_start/end. */
static void ut_dbg_setup_start_state(TCCState *s1, BufferedFile *bf, const char *filename)
{
  tcc_state = s1;
  text_section = new_section(s1, ".text", SHT_PROGBITS, SHF_EXECINSTR);
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);
  file = bf;
  strcpy(bf->filename, filename);
  bf->line_num = 1;
  bf->prev = NULL;
  s1->do_debug = 1;
}

UT_TEST(test_tcc_debug_start_dwarf4)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;

  tcc_debug_new(s1);
  tcc_debug_start(s1);

  UT_ASSERT(s1->dState != NULL);
  UT_ASSERT_EQ(s1->dwarf, 4);
  UT_ASSERT(dwarf_info_section->data_offset > 0);
  UT_ASSERT(dwarf_abbrev_section->data_offset > 0);
  UT_ASSERT(dwarf_line_section->data_offset > 0);
  UT_ASSERT(dwarf_aranges_section->data_offset == 0);
  UT_ASSERT(s1->dState->section_sym != 0);
  UT_ASSERT(s1->dState->dwarf_sym.info != 0);
  UT_ASSERT(s1->dState->dwarf_sym.line_str == s1->dState->dwarf_sym.str);
  UT_ASSERT(dwarf_line_str_section == dwarf_str_section);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_start_dwarf5)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 5;

  tcc_debug_new(s1);
  tcc_debug_start(s1);

  UT_ASSERT(dwarf_info_section->data_offset > 0);
  UT_ASSERT(dwarf_abbrev_section->data_offset > 0);
  UT_ASSERT(dwarf_line_section->data_offset > 0);
  UT_ASSERT(s1->dState->dwarf_sym.info != 0);
  UT_ASSERT(s1->dState->dwarf_sym.line_str != 0);
  UT_ASSERT(dwarf_line_str_section != dwarf_str_section);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_start_function_sections)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 5;
  s1->function_sections = 1;

  tcc_debug_new(s1);
  tcc_debug_start(s1);

  /* With -ffunction-sections the compile unit uses the ranges abbrev. */
  UT_ASSERT(dwarf_info_section->data_offset > 0);
  UT_ASSERT(dwarf_ranges_section->data_offset == 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_end_dwarf4)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  unsigned char text_data[16];

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  text_section->data = text_data;
  text_section->data_allocated = sizeof(text_data);
  text_section->data_offset = 4;

  tcc_debug_new(s1);
  tcc_debug_start(s1);
  ut_dbg_debug_end(s1);

  UT_ASSERT(dwarf_info_section->data_offset > 4);
  UT_ASSERT(dwarf_aranges_section->data_offset > 0);
  UT_ASSERT(dwarf_line_section->data_offset > 0);
  UT_ASSERT(dwarf_ranges_section->data_offset == 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_end_dwarf5)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  unsigned char text_data[16];

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 5;
  text_section->data = text_data;
  text_section->data_allocated = sizeof(text_data);
  text_section->data_offset = 4;

  tcc_debug_new(s1);
  tcc_debug_start(s1);
  ut_dbg_debug_end(s1);

  UT_ASSERT(dwarf_info_section->data_offset > 4);
  UT_ASSERT(dwarf_aranges_section->data_offset > 0);
  UT_ASSERT(dwarf_line_section->data_offset > 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_end_function_sections)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  unsigned char text_data[16];

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 5;
  s1->function_sections = 1;
  text_section->data = text_data;
  text_section->data_allocated = sizeof(text_data);
  text_section->data_offset = 4;

  tcc_debug_new(s1);
  tcc_debug_start(s1);
  ut_dbg_debug_end(s1);

  UT_ASSERT(dwarf_info_section->data_offset > 4);
  UT_ASSERT(dwarf_ranges_section->data_offset > 0);
  UT_ASSERT(dwarf_aranges_section->data_offset > 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_dwarf_file_cached_same_dir)
{
  TCCState *s1 = ut_dbg_make_state();
  BufferedFile bf = {0};

  file = &bf;
  s1->dwarf = 4;
  strcpy(bf.filename, "/x/y.c");
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 1);

  /* The first entry (index 0) is skipped by dwarf_file's cache scan, so a
   * second lookup of the same path allocates a second entry. */
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 2);
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_size, 2);

  /* The third lookup finds the j==1 entry created above and returns the same
   * file number, executing the cached-dir-and-name path at tccdbg.c:858-864. */
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 2);
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_size, 2);

  file = NULL;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_put_new_file_variants)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  BufferedFile prev = {0};
  BufferedFile *f;

  tcc_state = s1;
  text_section = new_section(s1, ".text", SHT_PROGBITS, SHF_EXECINSTR);
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);
  file = &bf;
  strcpy(bf.filename, "src.c");
  bf.prev = &prev;
  strcpy(prev.filename, "prev.c");
  s1->dwarf = 4;
  s1->dState->new_file = 0;

  /* new_file == 0 returns the (possibly prev-resolved) file unchanged. */
  f = put_new_file(s1);
  UT_ASSERT_EQ(f, &bf);
  UT_ASSERT_EQ(s1->dState->new_file, 0);

  /* new_file == 1 triggers dwarf_file for DWARF and resets the flag. */
  s1->dState->new_file = 1;
  f = put_new_file(s1);
  UT_ASSERT_EQ(f, &bf);
  UT_ASSERT_EQ(s1->dState->new_file, 0);

  /* ':asm:' filename resolves to the previous file. */
  strcpy(bf.filename, ":asm:");
  s1->dState->new_file = 1;
  f = put_new_file(s1);
  UT_ASSERT_EQ(f, &prev);

  /* Non-DWARF path uses put_stabs_r (a no-op in this build). */
  s1->dwarf = 0;
  strcpy(bf.filename, "src.c");
  s1->dState->new_file = 1;
  f = put_new_file(s1);
  UT_ASSERT_EQ(f, &bf);
  UT_ASSERT_EQ(s1->dState->new_file, 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_line_non_dwarf_func_ind_minus1)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Section text;
  unsigned char text_data[16];
  BufferedFile bf = {0};

  tcc_state = s1;
  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_flags = SHF_EXECINSTR;
  cur_text_section = &text;
  text_section = &text;
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);

  file = &bf;
  strcpy(bf.filename, "t.c");
  bf.line_num = 7;
  ind = 2;
  func_ind = -1;
  s1->do_debug = 1;
  s1->dwarf = 0;
  s1->ir = 0;
  nocode_wanted = 0;
  s1->dState->last_line_num = 0;

  tcc_debug_line(s1);
  UT_ASSERT_EQ(s1->dState->last_line_num, 7);

  file = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_line_num_non_dwarf)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Section text;
  unsigned char text_data[16];
  BufferedFile bf = {0};

  tcc_state = s1;
  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_flags = SHF_EXECINSTR;
  cur_text_section = &text;
  text_section = &text;
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);

  file = &bf;
  strcpy(bf.filename, "t.c");
  bf.line_num = 7;
  ind = 2;
  func_ind = 0;
  s1->do_debug = 1;
  s1->dwarf = 0;
  s1->ir = 0;
  nocode_wanted = 0;
  s1->dState->last_line_num = 0;

  tcc_debug_line_num(s1, 9);
  UT_ASSERT_EQ(s1->dState->last_line_num, 9);

  file = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_new_enables_backtrace_for_memory_output)
{
  TCCState *s1 = ut_dbg_make_state();

  s1->do_debug = 1;
  s1->output_type = TCC_OUTPUT_MEMORY;
  s1->dwarf = 0;
  tcc_debug_new(s1);

  UT_ASSERT_EQ(s1->do_backtrace, 1);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_start_non_dwarf_emits_stabs)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};

  tcc_state = s1;
  text_section = new_section(s1, ".text", SHT_PROGBITS, SHF_EXECINSTR);
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);
  file = &bf;
  strcpy(bf.filename, "/a/b.c");
  bf.line_num = 1;
  s1->do_debug = 1;
  s1->dwarf = 0;
  s1->nb_sections = 0;

  tcc_debug_new(s1);
  s1->dwarf = 0;
  tcc_debug_start(s1);

  UT_ASSERT(s1->dState != NULL);
  UT_ASSERT_EQ(s1->dState->section_sym, text_section->sh_num + 1);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_end_returns_when_debug_next_type_zero)
{
  TCCState *s1 = ut_dbg_make_state();

  s1->do_debug = 1;
  s1->dState->debug_next_type = 0;
  tcc_debug_end(s1);

  /* Reaching here without crashing is the test; no output is produced. */
  UT_ASSERT_EQ(s1->dState->debug_next_type, 0);

  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_stabn_creates_nested_scope)
{
  TCCState *s1 = ut_dbg_make_state();

  s1->do_debug = 1;
  tcc_debug_stabn(s1, N_LBRAC, 10);
  UT_ASSERT(s1->dState->debug_info != NULL);
  UT_ASSERT(s1->dState->debug_info_root != NULL);

  tcc_debug_stabn(s1, N_LBRAC, 20);
  UT_ASSERT(s1->dState->debug_info->parent == s1->dState->debug_info_root);

  tcc_debug_stabn(s1, N_RBRAC, 30);
  tcc_debug_stabn(s1, N_RBRAC, 40);

  UT_ASSERT(s1->dState->debug_info == NULL);
  UT_ASSERT(s1->dState->debug_info_root != NULL);
  UT_ASSERT_EQ(s1->dState->debug_info_root->end, 40);

  tcc_free(s1->dState->debug_info_root);
  s1->dState->debug_info_root = NULL;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_line_advance_pc_and_line)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Section text;
  unsigned char text_data[16];
  unsigned char line_data[64];
  BufferedFile bf = {0};
  int size_before;

  tcc_state = s1;
  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_flags = SHF_EXECINSTR;
  cur_text_section = &text;
  text_section = &text;
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);

  file = &bf;
  strcpy(bf.filename, "t.c");
  bf.line_num = 20;
  ind = 4;
  func_ind = -1;
  nocode_wanted = 0;
  s1->do_debug = 1;
  s1->dwarf = 4;
  s1->ir = 0;
  s1->dState->last_line_num = 0;

  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(sizeof(line_data));
  s1->dState->dwarf_line.line_max_size = sizeof(line_data);
  s1->dState->dwarf_line.line_size = 0;
  s1->dState->dwarf_line.cur_section = &text;
  s1->dState->dwarf_line.last_file = 1;
  s1->dState->dwarf_line.last_line = 1;
  s1->dState->dwarf_line.last_pc = 0;

  size_before = s1->dState->dwarf_line.line_size;
  tcc_debug_line(s1);
  UT_ASSERT(s1->dState->dwarf_line.line_size > size_before);

  file = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_line_num_special_opcode)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Section text;
  unsigned char text_data[16];
  unsigned char line_data[64];
  int size_before;

  tcc_state = s1;
  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_flags = SHF_EXECINSTR;
  cur_text_section = &text;
  text_section = &text;
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);

  file = NULL;
  ind = 4;
  func_ind = -1;
  nocode_wanted = 0;
  s1->do_debug = 1;
  s1->dwarf = 4;
  s1->ir = 0;
  s1->dState->last_line_num = 0;

  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(sizeof(line_data));
  s1->dState->dwarf_line.line_max_size = sizeof(line_data);
  s1->dState->dwarf_line.line_size = 0;
  s1->dState->dwarf_line.cur_section = &text;
  s1->dState->dwarf_line.last_file = 1;
  s1->dState->dwarf_line.last_line = 1;
  s1->dState->dwarf_line.last_pc = 0;

  size_before = s1->dState->dwarf_line.line_size;
  tcc_debug_line_num(s1, 5);
  UT_ASSERT(s1->dState->dwarf_line.line_size > size_before);
  UT_ASSERT_EQ(s1->dState->last_line_num, 5);

  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_line_num_non_dwarf_func_ind)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Section text;
  unsigned char text_data[16];
  BufferedFile bf = {0};

  tcc_state = s1;
  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_flags = SHF_EXECINSTR;
  cur_text_section = &text;
  text_section = &text;
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);

  file = &bf;
  strcpy(bf.filename, "t.c");
  bf.line_num = 11;
  ind = 6;
  func_ind = 2;
  s1->do_debug = 1;
  s1->dwarf = 0;
  s1->ir = 0;
  nocode_wanted = 0;
  s1->dState->last_line_num = 0;

  tcc_debug_line_num(s1, 11);
  UT_ASSERT_EQ(s1->dState->last_line_num, 11);

  file = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_funcend_non_dwarf)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Section text;
  unsigned char text_data[16];
  BufferedFile bf = {0};
  Sym func_sym = {0};
  Sym ret_sym = {0};

  tcc_state = s1;
  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_flags = SHF_EXECINSTR;
  text.sh_num = 1;
  cur_text_section = &text;
  text_section = &text;
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);

  file = &bf;
  strcpy(bf.filename, "t.c");
  bf.line_num = 1;
  funcname = "nf";
  ind = 2;
  func_ind = 0;
  s1->do_debug = 1;
  s1->dwarf = 0;
  s1->ir = 0;

  func_sym.type.t = VT_FUNC;
  func_sym.type.ref = &ret_sym;
  ret_sym.type.t = VT_INT;

  tcc_debug_funcstart(s1, &func_sym);
  UT_ASSERT(s1->dState->debug_info != NULL);

  tcc_debug_funcend(s1, 10);
  UT_ASSERT(s1->dState->debug_info == NULL);

  file = NULL;
  funcname = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_extern_sym_dwarf)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym sym = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  s1->do_debug = 3;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  sym.v = 100;
  sym.type.t = VT_INT;
  tcc_state = s1;
  utb_set_tok_str(100, "x");

  tcc_debug_extern_sym(s1, &sym, 0, STB_GLOBAL, STT_OBJECT);

  UT_ASSERT(dwarf_info_section->data_offset > 0);

  utb_set_tok_str(100, NULL);
  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_extern_sym_stabs)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym sym = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 0;
  s1->do_debug = 3;
  tcc_debug_new(s1);
  s1->dwarf = 0;
  tcc_debug_start(s1);

  common_section = new_section(s1, ".common", SHT_NOBITS, SHF_ALLOC);

  sym.v = 101;
  sym.type.t = VT_INT;
  tcc_state = s1;
  utb_set_tok_str(101, "y");

  tcc_debug_extern_sym(s1, &sym, SHN_COMMON, STB_LOCAL, STT_OBJECT);

  common_section = NULL;
  utb_set_tok_str(101, NULL);
  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_typedef_dwarf)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym sym = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  s1->do_debug = 3;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  sym.v = 102;
  sym.type.t = VT_INT;
  tcc_state = s1;
  utb_set_tok_str(102, "myint");

  tcc_debug_typedef(s1, &sym);

  UT_ASSERT(dwarf_info_section->data_offset > 0);

  utb_set_tok_str(102, NULL);
  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_typedef_stabs)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym sym = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 0;
  s1->do_debug = 3;
  tcc_debug_new(s1);
  s1->dwarf = 0;
  tcc_debug_start(s1);

  sym.v = 103;
  sym.type.t = VT_INT;
  tcc_state = s1;
  utb_set_tok_str(103, "yourint");

  tcc_debug_typedef(s1, &sym);

  utb_set_tok_str(103, NULL);
  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_debug_info_stabs_pointer)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Sym base = {0};
  Sym ptr = {0};
  CString result;

  tcc_state = s1;
  s1->dwarf = 0;
  base.type.t = VT_INT;
  ptr.type.t = VT_PTR;
  ptr.type.ref = &base;

  cstr_new(&result);
  tcc_get_debug_info(s1, &ptr, &result);
  UT_ASSERT(result.size > 0);

  cstr_free(&result);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_debug_info_stabs_struct)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Sym struct_def = {0};
  Sym field = {0};
  Sym s = {0};
  CString result;

  tcc_state = s1;
  struct_def.type.t = VT_STRUCT;
  struct_def.c = 4;
  struct_def.v = 200;
  field.type.t = VT_INT;
  field.v = 201;
  field.c = 0;
  struct_def.next = &field;

  s.type.t = VT_STRUCT;
  s.type.ref = &struct_def;

  utb_set_tok_str(200, "S");
  utb_set_tok_str(201, "f");

  s1->dwarf = 0;
  cstr_new(&result);
  tcc_get_debug_info(s1, &s, &result);
  UT_ASSERT(result.size > 0);

  cstr_free(&result);
  utb_set_tok_str(200, NULL);
  utb_set_tok_str(201, NULL);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_dwarf_info_base_type)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym sym = {0};
  int type;

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  sym.type.t = VT_INT;
  tcc_state = s1;

  type = tcc_get_dwarf_info(s1, &sym);
  UT_ASSERT(type > 0);
  UT_ASSERT(dwarf_info_section->data_offset > 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_dwarf_info_struct)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym struct_def = {0};
  Sym field = {0};
  Sym s = {0};
  int type;

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  struct_def.type.t = VT_STRUCT;
  struct_def.c = 4;
  struct_def.v = 300;
  field.type.t = VT_INT;
  field.v = 301;
  field.c = 0;
  struct_def.next = &field;

  s.type.t = VT_STRUCT;
  s.type.ref = &struct_def;

  tcc_state = s1;
  utb_set_tok_str(300, "S2");
  utb_set_tok_str(301, "f2");

  type = tcc_get_dwarf_info(s1, &s);
  UT_ASSERT(type > 0);
  UT_ASSERT(dwarf_info_section->data_offset > 0);

  utb_set_tok_str(300, NULL);
  utb_set_tok_str(301, NULL);
  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_dwarf_info_enum)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym enum_def = {0};
  Sym member = {0};
  Sym s = {0};
  int type;

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  enum_def.type.t = VT_ENUM;
  enum_def.v = 400;
  member.type.t = VT_INT | VT_ENUM_VAL;
  member.v = 401;
  member.enum_val = 1;
  enum_def.next = &member;

  s.type.t = VT_ENUM;
  s.type.ref = &enum_def;

  tcc_state = s1;
  utb_set_tok_str(400, "E");
  utb_set_tok_str(401, "M");

  type = tcc_get_dwarf_info(s1, &s);
  UT_ASSERT(type > 0);
  UT_ASSERT(dwarf_info_section->data_offset > 0);

  utb_set_tok_str(400, NULL);
  utb_set_tok_str(401, NULL);
  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_dwarf_info_pointer)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym base = {0};
  Sym ptr = {0};
  int type;

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  base.type.t = VT_INT;
  ptr.type.t = VT_PTR;
  ptr.type.ref = &base;

  tcc_state = s1;
  type = tcc_get_dwarf_info(s1, &ptr);
  UT_ASSERT(type > 0);
  UT_ASSERT(dwarf_info_section->data_offset > 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_dwarf_info_array)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym base = {0};
  Sym arr = {0};
  int type;

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  base.type.t = VT_INT;
  base.c = 10;
  arr.type.t = VT_PTR | VT_ARRAY;
  arr.type.ref = &base;

  tcc_state = s1;
  type = tcc_get_dwarf_info(s1, &arr);
  UT_ASSERT(type > 0);
  UT_ASSERT(dwarf_info_section->data_offset > 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_dwarf_info_func)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym ret = {0};
  Sym func = {0};
  Sym param = {0};
  Sym s = {0};
  int type;

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  ret.type.t = VT_INT;
  func.type.t = VT_FUNC;
  func.type.ref = &ret;
  func.next = &param;
  param.type.t = VT_INT;
  param.v = 500;

  s.type.t = VT_FUNC;
  s.type.ref = &func;

  tcc_state = s1;
  utb_set_tok_str(500, "p");

  type = tcc_get_dwarf_info(s1, &s);
  UT_ASSERT(type > 0);
  UT_ASSERT(dwarf_info_section->data_offset > 0);

  utb_set_tok_str(500, NULL);
  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_fix_anon)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym anon = {0};
  Sym field = {0};
  CType t;

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->do_debug = 2;
  s1->dwarf = 4;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  /* Create an anonymous struct entry in the anon hash. */
  anon.type.t = VT_STRUCT;
  anon.c = -1;
  tcc_debug_find(s1, &anon, 1);

  /* Record a reference to the anon struct from a field. */
  field.type.t = VT_STRUCT;
  field.type.ref = &anon;
  tcc_debug_check_anon(s1, &field, dwarf_info_section->data_offset);

  /* Simulate the struct becoming fully defined (size known). */
  anon.c = 4;

  /* Now fix the anon struct. */
  t.t = VT_STRUCT;
  t.ref = &anon;

  tcc_state = s1;
  tcc_debug_fix_anon(s1, &t);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_end_anon_hash_fixes_refs)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym anon = {0};
  Sym field = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->do_debug = 2;
  s1->dwarf = 4;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  /* Create an anonymous struct entry in the anon hash. */
  anon.type.t = VT_STRUCT;
  anon.c = -1;
  tcc_debug_find(s1, &anon, 1);

  /* Record a reference to the anon struct from a field. */
  field.type.t = VT_STRUCT;
  field.type.ref = &anon;
  tcc_debug_check_anon(s1, &field, dwarf_info_section->data_offset);

  /* Simulate the struct becoming fully defined. */
  anon.c = 4;

  tcc_state = s1;
  ut_dbg_debug_end(s1);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_end_function_sections_with_text)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Section *text;

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 5;
  s1->function_sections = 1;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  text = new_section(s1, ".text.func", SHT_PROGBITS, SHF_EXECINSTR);
  text->data_offset = 8;
  text_section->data_offset = 4;

  tcc_state = s1;
  dwarf_register_text_section(s1, text);

  ut_dbg_debug_end(s1);

  UT_ASSERT(dwarf_ranges_section->data_offset > 0);
  UT_ASSERT(dwarf_aranges_section->data_offset > 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_finish_non_dwarf_with_symbols)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Section text;
  unsigned char text_data[16];
  BufferedFile bf = {0};
  Sym func_sym = {0};
  Sym ret_sym = {0};

  tcc_state = s1;
  test_section_reset(&text, text_data, sizeof(text_data));
  text.s1 = s1;
  text.sh_flags = SHF_EXECINSTR;
  text.sh_num = 1;
  cur_text_section = &text;
  text_section = &text;
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);

  file = &bf;
  strcpy(bf.filename, "t.c");
  bf.line_num = 1;
  funcname = "nf";
  ind = 2;
  func_ind = 0;
  s1->do_debug = 3;
  s1->dwarf = 0;
  s1->ir = 0;

  func_sym.type.t = VT_FUNC;
  func_sym.type.ref = &ret_sym;
  ret_sym.type.t = VT_INT;

  tcc_debug_funcstart(s1, &func_sym);
  /* Add a symbol to the current scope so tcc_debug_finish has work. */
  tcc_debug_stabs(s1, "local_var", N_LSYM, 4, NULL, 0, 0, -1, 0);

  tcc_debug_funcend(s1, 10);
  UT_ASSERT(s1->dState->debug_info == NULL);

  file = NULL;
  funcname = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_funcstart_do_backtrace)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Section info_sec, str_sec, text_sec, symtab;
  unsigned char info_data[1024];
  unsigned char str_data[256];
  unsigned char text_data[16];
  unsigned char symtab_data[16];
  Sym func_sym = {0};
  Sym ret_sym = {0};
  BufferedFile bf = {0};

  test_section_reset(&info_sec, info_data, sizeof(info_data));
  test_section_reset(&str_sec, str_data, sizeof(str_data));
  test_section_reset(&text_sec, text_data, sizeof(text_data));
  test_section_reset(&symtab, symtab_data, sizeof(symtab_data));
  info_sec.s1 = s1;
  str_sec.s1 = s1;
  text_sec.s1 = s1;
  text_sec.sh_flags = SHF_EXECINSTR;
  text_sec.sh_num = 1;

  tcc_state = s1;
  dwarf_info_section = &info_sec;
  dwarf_str_section = &str_sec;
  cur_text_section = &text_sec;
  text_section = &text_sec;
  symtab_section = &symtab;

  s1->do_debug = 1;
  s1->dwarf = 4;
  s1->ir = 0;
  s1->do_backtrace = 1;

  funcname = "bt_fn";
  file = &bf;
  strcpy(bf.filename, "bt.c");
  bf.line_num = 1;

  ind = 2;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  s1->dState->dwarf_line.cur_section = NULL;
  s1->dState->dwarf_line.last_file = 0;
  s1->dState->dwarf_line.last_line = 0;
  s1->dState->dwarf_line.last_pc = 0;
  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(256);
  s1->dState->dwarf_line.line_max_size = 256;
  s1->dState->dwarf_line.line_size = 0;

  func_sym.type.t = VT_FUNC;
  func_sym.type.ref = &ret_sym;
  ret_sym.type.t = VT_INT;

  tcc_debug_funcstart(s1, &func_sym);
  UT_ASSERT(s1->dState->dwarf_line.line_size > 0);

  tcc_state = old_tcc;
  file = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_save_restore_state_null_dstate)
{
  TCCState *s1 = ut_dbg_make_state();
  void *saved_info = (void *)1;
  void *saved_root = (void *)2;

  tcc_free(s1->dState);
  s1->dState = NULL;

  tcc_debug_save_state(s1, &saved_info, &saved_root);
  UT_ASSERT_EQ(saved_info, (void *)NULL);
  UT_ASSERT_EQ(saved_root, (void *)NULL);

  tcc_debug_restore_state(s1, (void *)3, (void *)4);
  /* No crash is the success criterion. */

  /* Re-create dState so ut_dbg_free_state can clean up. */
  s1->dState = (struct _tccdbg *)tcc_mallocz(sizeof(*s1->dState));
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_debug_info_enum_stabs)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Sym enum_def = {0};
  Sym member = {0};
  Sym s = {0};
  CString result;

  tcc_state = s1;
  enum_def.type.t = VT_ENUM;
  enum_def.v = 300;
  member.type.t = VT_INT | VT_ENUM_VAL;
  member.v = 301;
  member.enum_val = 42;
  enum_def.next = &member;

  s.type.t = VT_ENUM;
  s.type.ref = &enum_def;

  utb_set_tok_str(300, "Color");
  utb_set_tok_str(301, "Red");

  cstr_new(&result);
  tcc_get_debug_info(s1, &s, &result);
  UT_ASSERT(result.size > 0);

  cstr_free(&result);
  utb_set_tok_str(300, NULL);
  utb_set_tok_str(301, NULL);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_debug_info_array_stabs)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Sym base = {0};
  Sym arr = {0};
  CString result;

  tcc_state = s1;
  base.type.t = VT_INT;
  base.c = 10;
  arr.type.t = VT_PTR | VT_ARRAY;
  arr.type.ref = &base;

  cstr_new(&result);
  tcc_get_debug_info(s1, &arr, &result);
  UT_ASSERT(result.size > 0);

  cstr_free(&result);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_debug_info_func_stabs)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Sym ret = {0};
  Sym func = {0};
  Sym s = {0};
  CString result;

  tcc_state = s1;
  ret.type.t = VT_INT;
  func.type.t = VT_FUNC;
  func.type.ref = &ret;

  s.type.t = VT_FUNC;
  s.type.ref = &func;

  cstr_new(&result);
  tcc_get_debug_info(s1, &s, &result);
  UT_ASSERT(result.size > 0);

  cstr_free(&result);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_end_non_dwarf_closes_so)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  unsigned char text_data[16];

  tcc_state = s1;
  text_section = new_section(s1, ".text", SHT_PROGBITS, SHF_EXECINSTR);
  symtab_section = new_section(s1, ".symtab", SHT_SYMTAB, 0);
  text_section->data = text_data;
  text_section->data_allocated = sizeof(text_data);
  text_section->data_offset = 4;

  file = &bf;
  strcpy(bf.filename, "/a/b.c");
  bf.line_num = 1;
  s1->do_debug = 1;
  s1->dwarf = 0;

  tcc_debug_new(s1);
  s1->dwarf = 0;
  tcc_debug_start(s1);
  ut_dbg_debug_end(s1);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_start_dwarf3_form_conversion)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 3;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  UT_ASSERT(dwarf_info_section->data_offset > 0);
  UT_ASSERT(dwarf_abbrev_section->data_offset > 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_dwarf_info_struct_with_bitfield)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym struct_def = {0};
  Sym field = {0};
  Sym s = {0};
  int type;

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  struct_def.type.t = VT_STRUCT;
  struct_def.c = 4;
  struct_def.v = 500;
  field.type.t = VT_INT | VT_BITFIELD;
  field.type.t |= (3 << VT_STRUCT_SHIFT) | (1 << VT_STRUCT_SHIFT);
  field.v = 501;
  field.c = 0;
  struct_def.next = &field;

  s.type.t = VT_STRUCT;
  s.type.ref = &struct_def;

  tcc_state = s1;
  utb_set_tok_str(500, "S");
  utb_set_tok_str(501, "f");

  type = tcc_get_dwarf_info(s1, &s);
  UT_ASSERT(type > 0);

  utb_set_tok_str(500, NULL);
  utb_set_tok_str(501, NULL);
  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_debug_info_stabs_struct_with_bitfield)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Sym struct_def = {0};
  Sym field = {0};
  Sym s = {0};
  CString result;

  tcc_state = s1;
  s1->dwarf = 0;
  struct_def.type.t = VT_STRUCT;
  struct_def.c = 4;
  struct_def.v = 510;
  field.type.t = VT_INT | VT_BITFIELD;
  field.type.t |= (3 << VT_STRUCT_SHIFT) | (1 << VT_STRUCT_SHIFT);
  field.v = 511;
  field.c = 0;
  struct_def.next = &field;

  s.type.t = VT_STRUCT;
  s.type.ref = &struct_def;

  utb_set_tok_str(510, "SB");
  utb_set_tok_str(511, "bf");

  cstr_new(&result);
  tcc_get_debug_info(s1, &s, &result);
  UT_ASSERT(result.size > 0);

  cstr_free(&result);
  utb_set_tok_str(510, NULL);
  utb_set_tok_str(511, NULL);
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_get_dwarf_info_pointer_to_pointer)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym base = {0};
  Sym inner = {0};
  Sym outer = {0};
  int type;

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  base.type.t = VT_INT;
  inner.type.t = VT_PTR;
  inner.type.ref = &base;
  outer.type.t = VT_PTR;
  outer.type.ref = &inner;

  tcc_state = s1;
  type = tcc_get_dwarf_info(s1, &outer);
  UT_ASSERT(type > 0);
  UT_ASSERT(dwarf_info_section->data_offset > 0);

  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_debug_funcend_dwarf_with_local)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  Section info_sec, str_sec, text_sec, symtab;
  unsigned char info_data[1024];
  unsigned char str_data[256];
  unsigned char text_data[16];
  unsigned char symtab_data[16];
  Sym func_sym = {0};
  Sym ret_sym = {0};
  BufferedFile bf = {0};

  test_section_reset(&info_sec, info_data, sizeof(info_data));
  test_section_reset(&str_sec, str_data, sizeof(str_data));
  test_section_reset(&text_sec, text_data, sizeof(text_data));
  test_section_reset(&symtab, symtab_data, sizeof(symtab_data));
  info_sec.s1 = s1;
  str_sec.s1 = s1;
  text_sec.s1 = s1;
  text_sec.sh_flags = SHF_EXECINSTR;
  text_sec.sh_num = 1;

  tcc_state = s1;
  dwarf_info_section = &info_sec;
  dwarf_str_section = &str_sec;
  cur_text_section = &text_sec;
  text_section = &text_sec;
  symtab_section = &symtab;

  s1->do_debug = 1;
  s1->dwarf = 4;
  s1->ir = 0;

  funcname = "df";
  file = &bf;
  strcpy(bf.filename, "df.c");
  bf.line_num = 1;

  ind = 2;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  s1->dState->dwarf_info.start = 0;
  s1->dState->dwarf_sym.str = 0;
  s1->dState->dwarf_sym.line_str = 0;
  s1->dState->dwarf_line.cur_section = NULL;
  s1->dState->dwarf_line.last_file = 0;
  s1->dState->dwarf_line.last_line = 0;
  s1->dState->dwarf_line.last_pc = 0;
  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(256);
  s1->dState->dwarf_line.line_max_size = 256;
  s1->dState->dwarf_line.line_size = 0;

  func_sym.type.t = VT_FUNC;
  func_sym.type.ref = &ret_sym;
  ret_sym.type.t = VT_INT;

  tcc_debug_funcstart(s1, &func_sym);
  tcc_debug_stabs(s1, "lv", N_LSYM, 4, NULL, 0, 0, -1, 0);
  tcc_debug_funcend(s1, 10);

  tcc_state = old_tcc;
  file = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_add_debug_info_dwarf)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym a = {0};
  Sym b = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 4;
  s1->do_debug = 3;
  tcc_debug_new(s1);
  tcc_debug_start(s1);

  a.v = 600;
  a.type.t = VT_INT;
  a.c = 0;
  a.r = VT_LOCAL;
  a.prev = &b;
  b.v = 601;
  b.type.t = VT_INT;
  b.c = 4;
  b.r = VT_LOCAL;
  b.prev = NULL;

  tcc_state = s1;
  utb_set_tok_str(600, "a");
  utb_set_tok_str(601, "b");

  tcc_add_debug_info(s1, 0, &a, NULL);
  UT_ASSERT(dwarf_info_section->data_offset > 0);

  utb_set_tok_str(600, NULL);
  utb_set_tok_str(601, NULL);
  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}

UT_TEST(test_tcc_add_debug_info_stabs)
{
  TCCState *s1 = ut_dbg_make_state();
  TCCState *old_tcc = tcc_state;
  BufferedFile bf = {0};
  Sym a = {0};
  Sym b = {0};

  ut_dbg_setup_start_state(s1, &bf, "/a/b.c");
  s1->dwarf = 0;
  s1->do_debug = 3;
  tcc_debug_new(s1);
  s1->dwarf = 0;
  tcc_debug_start(s1);

  a.v = 602;
  a.type.t = VT_INT;
  a.c = 0;
  a.r = VT_LOCAL;
  a.prev = &b;
  b.v = 603;
  b.type.t = VT_INT;
  b.c = 4;
  b.r = VT_LOCAL;
  b.prev = NULL;

  tcc_state = s1;
  utb_set_tok_str(602, "x");
  utb_set_tok_str(603, "y");

  tcc_add_debug_info(s1, 0, &a, NULL);

  utb_set_tok_str(602, NULL);
  utb_set_tok_str(603, NULL);
  file = NULL;
  tcc_state = old_tcc;
  ut_dbg_free_state(s1);
  return 0;
}
