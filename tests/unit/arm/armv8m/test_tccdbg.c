/*
 *  test_tccdbg.c - suite for tccdbg.c DWARF helper routines
 */

#define USING_GLOBALS
#include "tcc.h"
#include "ut.h"

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
#include "tccdbg.c"

/* tccdbg.c #undef's its USING_GLOBALS helper macros at EOF.  The tests below
 * need the same shorthand, so restore the ones they touch. */
#define s1->dState->last_line_num       s1->dState->last_line_num
#define debug_hash          s1->dState->debug_hash
#define s1->dState->n_debug_hash        s1->dState->n_debug_hash
#define s1->dState->n_debug_anon_hash   s1->dState->n_debug_anon_hash
#define dwarf_line          s1->dState->dwarf_line
#define dwarf_info          s1->dState->dwarf_info

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
    /* These macros remain defined after the #include. */
    tcc_free(dwarf_text_sections);
    tcc_free(dwarf_line_relocs);
    /* debug hash macros are #undef'd. */
    tcc_free(s1->dState->debug_hash);
    if (s1->dState->debug_anon_hash)
    {
      for (i = 0; i < s1->dState->n_debug_anon_hash; i++)
        tcc_free(s1->dState->debug_anon_hash[i].debug_type);
      tcc_free(s1->dState->debug_anon_hash);
    }
    tcc_free(s1->dState);
  }
  tcc_free(s1);
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
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(-1), 11);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(0), 1);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(31), 1);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(32), 2);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(16383), 3);
  UT_ASSERT_EQ(dwarf_loc_reg_op_len(16384), 4);
  return 0;
}

UT_TEST(test_dwarf_emit_reg_op_negative_reg_encodes_as_regx)
{
  Section sec;
  unsigned char data[32];

  test_section_reset(&sec, data, sizeof(data));
  dwarf_emit_reg_op(&sec, -1);
  UT_ASSERT_EQ(sec.data[0], DW_OP_regx);
  UT_ASSERT_EQ(sec.data_offset, 11); /* DW_OP_regx + 10-byte uleb128 */

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

  /* Cached lookup for the same path. */
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_size, 1);

  /* Same directory, new basename. */
  strcpy(bf.filename, name2);
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 2);
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_size, 2);

  /* Baseline file with no directory. */
  strcpy(bf.filename, name3);
  dwarf_file(s1);
  UT_ASSERT_EQ(s1->dState->dwarf_line.cur_file, 3);
  UT_ASSERT_EQ(s1->dState->dwarf_line.filename_table[2].dir_entry, 0);

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

  tcc_free(s1->dState->dwarf_line.line_data);
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
  text.sh_flags = SHF_EXECINSTR;
  tcc_state = s1;
  cur_text_section = &text;
  s1->dwarf = 4;
  s1->do_debug = 1;
  s1->ir = 0;

  file = &bf;
  strcpy(bf.filename, "test.c");
  bf.line_num = 10;
  ind = 2;
  func_ind = -1;
  nocode_wanted = 0;
  s1->dState->last_line_num = 1;

  s1->dState->dwarf_line.line_data = (unsigned char *)tcc_malloc(sizeof(line_data));
  s1->dState->dwarf_line.line_max_size = sizeof(line_data);
  s1->dState->dwarf_line.line_size = 0;
  s1->dState->dwarf_line.cur_section = NULL;
  s1->dState->dwarf_line.last_file = 0;
  s1->dState->dwarf_line.last_line = 1;
  s1->dState->dwarf_line.last_pc = 0;

  tcc_debug_line(s1);

  UT_ASSERT(s1->dState->dwarf_line.line_size > 0);
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[2], DW_LNE_set_address);
  idx = 3 + PTR_SIZE;
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[idx], DW_LNS_set_file);
  /* cur_file == 1, so the uleb128 size is one byte. */
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[idx + 1], 1);
  /* Special opcode: 1*14 + (10-1) + 13 - (-5) == 41. */
  UT_ASSERT_EQ(s1->dState->dwarf_line.line_data[idx + 2], 41);

  tcc_free(s1->dState->dwarf_line.line_data);
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

  tcc_free(s1->dState->dwarf_line.line_data);
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

  tcc_free(s1->dState->dwarf_line.line_data);
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

  tcc_free(s1->dState->dwarf_line.line_data);
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

  tcc_free(s1->dState->dwarf_line.line_data);
  tcc_state = old_tcc;
  file = NULL;
  ind = 0;
  func_ind = 0;
  s1->dState->last_line_num = 0;
  ut_dbg_free_state(s1);
  return 0;
}

UT_SUITE(tccdbg)
{
  UT_RUN(test_dwarf_uleb128_size_boundaries);
  UT_RUN(test_dwarf_sleb128_size_boundaries);
  UT_RUN(test_dwarf_uleb128_encoding);
  UT_RUN(test_dwarf_sleb128_encoding);
  UT_RUN(test_dwarf_emit_reg_op_uses_short_form_for_reg0_to_reg31);
  UT_RUN(test_dwarf_emit_reg_op_uses_regx_for_large_registers);
  UT_RUN(test_dwarf_reg_piece_size_for_sym);
  UT_RUN(test_dwarf_emit_regpair_expr);
  UT_RUN(test_dwarf_line_op_grows_buffer);
  UT_RUN(test_dwarf_uleb128_op_encoding);
  UT_RUN(test_dwarf_sleb128_op_encoding);
  UT_RUN(test_dwarf_get_section_sym_returns_symbol_index);
  UT_RUN(test_dwarf_register_text_section_tracks_unique_sections);
  UT_RUN(test_dwarf_add_line_reloc_grows_array);
  UT_RUN(test_tcc_debug_find_add_remove);
  UT_RUN(test_tcc_debug_check_anon_records_offsets);
  UT_RUN(test_tcc_debug_stabs_accumulates_symbols);
  UT_RUN(test_tcc_debug_stabs_without_debug_info_calls_put_stabs);
  UT_RUN(test_tcc_debug_stabn_builds_scope_tree);
  UT_RUN(test_tcc_debug_save_restore_state);
  UT_RUN(test_dwarf_loc_reg_op_len_edge_cases);
  UT_RUN(test_dwarf_emit_reg_op_negative_reg_encodes_as_regx);
  UT_RUN(test_dwarf_file_tracks_paths);
  UT_RUN(test_dwarf_file_dwarf5_index_offset);
  UT_RUN(test_dwarf_strp_appends_string_with_relocation_skipped);
  UT_RUN(test_dwarf_line_strp_appends_string);
  UT_RUN(test_dwarf_reloc_skips_invalid_symbol_index);
  UT_RUN(test_put_stabs_variants_are_no_ops);
  UT_RUN(test_dwarf_emit_set_address_encodes_extended_opcode);
  UT_RUN(test_tcc_debug_line_emits_dwarf_special_opcode);
  UT_RUN(test_tcc_debug_line_skips_non_executable_section);
  UT_RUN(test_tcc_debug_line_num_emits_and_dedupes);
  UT_RUN(test_tcc_debug_newfile_bincl_eincl);
  UT_RUN(test_tcc_debug_prolog_epilog_emits_flags);
  UT_RUN(test_tcc_debug_find_returns_zero_for_existing_anon);
  UT_RUN(test_tcc_debug_remove_shifts_remaining_entries);
  UT_RUN(test_tcc_debug_funcstart_and_funcend_dwarf);
}
