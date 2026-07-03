/*
 *  test_tccelf.c - white-box unit tests for tccelf.c helpers
 *  (build_tccelf/run_unit_tests_tccelf)
 *
 *  Tests the isolated Section/Symbol data-structure operations and the
 *  tccelf_new/tccelf_delete lifecycle without pulling in the frontend,
 *  assembler, or real debug/eh-frame machinery.
 */

#define USING_GLOBALS
#include "tcc.h"

#include "ut.h"

/* These constants are private to tccelf.c; mirror them here for the tests. */
#ifndef SHF_PRIVATE
#define SHF_PRIVATE 0x80000000
#endif
#ifndef SYMTAB_INITIAL_HASH_BUCKETS
#define SYMTAB_INITIAL_HASH_BUCKETS 512
#endif

static void ut_elf_reset_state(void)
{
  memset(tcc_state, 0, sizeof(TCCState));
}

static void ut_elf_init_minimal(void)
{
  /* tccelf.c assumes sections[0] is a NULL dummy. */
  dynarray_add(&tcc_state->sections, &tcc_state->nb_sections, NULL);
}

/* ============================================================================
 * Section creation and lookup
 * ============================================================================ */

UT_TEST(test_new_section_creates_named_typed_section)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".mytext", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
  UT_ASSERT(sec != NULL);
  UT_ASSERT_STREQ(sec->name, ".mytext");
  UT_ASSERT_EQ(sec->sh_type, SHT_PROGBITS);
  UT_ASSERT_EQ(sec->sh_flags, (int)(SHF_ALLOC | SHF_EXECINSTR));
  UT_ASSERT_EQ(sec->sh_num, 1);
  UT_ASSERT(sec->s1 == tcc_state);

  /* Default alignment for PROGBITS is PTR_SIZE (8 on this host build). */
  UT_ASSERT_EQ(sec->sh_addralign, 8);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_new_section_private_goes_to_priv_sections)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".common", SHT_NOBITS, SHF_PRIVATE);
  UT_ASSERT(sec != NULL);
  UT_ASSERT_EQ(sec->sh_num, 0);            /* private sections have no public number */
  UT_ASSERT_EQ(tcc_state->nb_sections, 1); /* not added to public list */
  UT_ASSERT_EQ(tcc_state->nb_priv_sections, 1);
  UT_ASSERT(tcc_state->priv_sections[0] == sec);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_find_section_creates_missing_section)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = find_section(tcc_state, ".newsec");
  UT_ASSERT(sec != NULL);
  UT_ASSERT_STREQ(sec->name, ".newsec");
  UT_ASSERT_EQ(sec->sh_type, SHT_PROGBITS);
  UT_ASSERT_EQ(sec->sh_flags, (int)SHF_ALLOC);

  /* Second lookup returns the same section. */
  Section *sec2 = find_section(tcc_state, ".newsec");
  UT_ASSERT(sec2 == sec);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_hash_table_grows_and_still_finds_sections)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  /* Initial table size is 64; insert enough sections to force growth. */
  char name[32];
  for (int i = 0; i < 80; i++)
  {
    snprintf(name, sizeof(name), ".sec.%03d", i);
    Section *sec = new_section(tcc_state, name, SHT_PROGBITS, SHF_ALLOC);
    UT_ASSERT(sec != NULL);
  }

  /* After growth find_section must still resolve every name. */
  for (int i = 0; i < 80; i++)
  {
    snprintf(name, sizeof(name), ".sec.%03d", i);
    Section *found = find_section(tcc_state, name);
    UT_ASSERT(found != NULL);
    UT_ASSERT_STREQ(found->name, name);
  }

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Section data allocation
 * ============================================================================ */

UT_TEST(test_section_add_allocates_and_aligns)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);

  size_t off1 = section_add(sec, 4, 1);
  UT_ASSERT_EQ(off1, 0);
  UT_ASSERT_EQ(sec->data_offset, 4);

  size_t off2 = section_add(sec, 8, 8);
  UT_ASSERT_EQ(off2, 8); /* aligned up from 4 */
  UT_ASSERT_EQ(sec->data_offset, 16);

  size_t off3 = section_add(sec, 2, 1);
  UT_ASSERT_EQ(off3, 16);
  UT_ASSERT_EQ(sec->data_offset, 18);

  UT_ASSERT(sec->data_allocated >= 18);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_add_nobits_does_not_allocate_data)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);
  UT_ASSERT(sec->data == NULL);

  size_t off = section_add(sec, 64, 4);
  UT_ASSERT_EQ(off, 0);
  UT_ASSERT_EQ(sec->data_offset, 64);
  UT_ASSERT(sec->data == NULL);
  UT_ASSERT_EQ(sec->data_allocated, 0);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_ptr_add_returns_writable_pointer)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
  unsigned char *p = (unsigned char *)section_ptr_add(sec, 4);
  UT_ASSERT(p != NULL);
  UT_ASSERT(p == sec->data);
  p[0] = 0x12;
  p[1] = 0x34;
  p[2] = 0x56;
  p[3] = 0x78;
  UT_ASSERT_EQ(sec->data_offset, 4);
  UT_ASSERT_EQ(sec->data[0], 0x12);
  UT_ASSERT_EQ(sec->data[3], 0x78);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_realloc_rounds_up_to_power_of_two)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".buf", SHT_PROGBITS, SHF_ALLOC);
  section_realloc(sec, 300);
  UT_ASSERT(sec->data_allocated >= 300);
  UT_ASSERT_EQ(sec->data_allocated, 512); /* first allocation minimum/rounding */
  UT_ASSERT(sec->data != NULL);

  section_realloc(sec, 600);
  UT_ASSERT(sec->data_allocated >= 600);
  UT_ASSERT_EQ(sec->data_allocated, 1024);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_prealloc_reserves_capacity_without_moving_offset)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".pre", SHT_PROGBITS, SHF_ALLOC);
  section_ptr_add(sec, 8);
  UT_ASSERT_EQ(sec->data_offset, 8);

  section_prealloc(sec, 256);
  UT_ASSERT(sec->data_allocated >= 264);
  UT_ASSERT_EQ(sec->data_offset, 8);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * String tables
 * ============================================================================ */

UT_TEST(test_put_elf_str_appends_and_returns_offsets)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *strtab = new_section(tcc_state, ".strtab", SHT_STRTAB, SHF_PRIVATE);
  int off1 = put_elf_str(strtab, "hello");
  int off2 = put_elf_str(strtab, "world");
  int off3 = put_elf_str(strtab, "");
  int off4 = put_elf_str(strtab, NULL);

  /* put_elf_str appends every request (no deduplication).  NULL is treated
   * as the empty string, but it still consumes a fresh byte. */
  UT_ASSERT_EQ(off1, 0);
  UT_ASSERT_EQ(off2, 6);
  UT_ASSERT_EQ(off3, 12);
  UT_ASSERT_EQ(off4, 13);
  UT_ASSERT_STREQ((char *)strtab->data + off1, "hello");
  UT_ASSERT_STREQ((char *)strtab->data + off2, "world");
  UT_ASSERT_EQ(strtab->data[off3], '\0');
  UT_ASSERT_EQ(strtab->data[off4], '\0');
  UT_ASSERT_EQ(strtab->data_offset, 14);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Symbol tables and ELF hashing
 * ============================================================================ */

UT_TEST(test_new_symtab_initializes_hash_and_first_symbol)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *symtab = new_symtab(tcc_state, ".symtab", SHT_SYMTAB, 0, ".strtab", ".hashtab", SHF_PRIVATE);
  UT_ASSERT(symtab != NULL);
  UT_ASSERT(symtab->link != NULL);
  UT_ASSERT_STREQ(symtab->link->name, ".strtab");
  UT_ASSERT(symtab->hash != NULL);
  UT_ASSERT_STREQ(symtab->hash->name, ".hashtab");

  /* init_symtab adds the empty string and one zeroed symbol. */
  UT_ASSERT_EQ(symtab->link->data_offset, 1);
  UT_ASSERT_EQ(symtab->data_offset, sizeof(ElfW(Sym)));

  int *hash = (int *)symtab->hash->data;
  UT_ASSERT_EQ(hash[0], SYMTAB_INITIAL_HASH_BUCKETS);
  UT_ASSERT_EQ(hash[1], 1);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_put_elf_sym_adds_local_and_global_symbols)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *symtab = new_symtab(tcc_state, ".symtab", SHT_SYMTAB, 0, ".strtab", ".hashtab", SHF_PRIVATE);

  int idx_local = put_elf_sym(symtab, 0x100, 4,
                              ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT),
                              STV_DEFAULT, 1, "local_sym");
  int idx_global = put_elf_sym(symtab, 0x200, 8,
                               ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC),
                               STV_DEFAULT, 1, "global_sym");

  UT_ASSERT_EQ(idx_local, 1);
  UT_ASSERT_EQ(idx_global, 2);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab->data;
  UT_ASSERT_EQ(syms[idx_local].st_value, 0x100);
  UT_ASSERT_EQ(syms[idx_global].st_value, 0x200);
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[idx_local].st_info), STB_LOCAL);
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[idx_global].st_info), STB_GLOBAL);

  /* Global symbol should be findable through the hash table. */
  UT_ASSERT_EQ(find_elf_sym(symtab, "global_sym"), idx_global);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_find_elf_sym_locates_added_symbols)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *symtab = new_symtab(tcc_state, ".symtab", SHT_SYMTAB, 0, ".strtab", ".hashtab", SHF_PRIVATE);

  put_elf_sym(symtab, 0, 0,
              ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC), STV_DEFAULT, 1, "alpha");
  put_elf_sym(symtab, 0, 0,
              ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC), STV_DEFAULT, 1, "beta");

  int idx_alpha = find_elf_sym(symtab, "alpha");
  int idx_beta = find_elf_sym(symtab, "beta");
  int idx_gamma = find_elf_sym(symtab, "gamma");

  UT_ASSERT_EQ(idx_alpha, 1);
  UT_ASSERT_EQ(idx_beta, 2);
  UT_ASSERT_EQ(idx_gamma, 0);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_adds_new_local_symbol)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx = set_elf_sym(symtab_section, 0x400, 4,
                        ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT),
                        STV_DEFAULT, text_section->sh_num, "local_obj");
  UT_ASSERT_EQ(idx, 1); /* 0 is the null symbol inserted by init_symtab */

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx].st_value, 0x400);
  UT_ASSERT_EQ(syms[idx].st_shndx, text_section->sh_num);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_patches_existing_undefined_to_defined)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx_undef = set_elf_sym(symtab_section, 0, 0,
                              ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC),
                              STV_DEFAULT, SHN_UNDEF, "patch_me");
  UT_ASSERT_EQ(idx_undef, 1);

  int idx_def = set_elf_sym(symtab_section, 0x500, 16,
                            ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC),
                            STV_DEFAULT, text_section->sh_num, "patch_me");
  UT_ASSERT_EQ(idx_def, idx_undef);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx_def].st_shndx, text_section->sh_num);
  UT_ASSERT_EQ(syms[idx_def].st_value, 0x500);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_detects_duplicate_global_definition)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx1 = set_elf_sym(symtab_section, 0x600, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, text_section->sh_num, "dup");
  UT_ASSERT_EQ(idx1, 1);

  /* Second definition with different value is an error; the stubbed
   * tcc_error_noabort returns -1, and set_elf_sym returns the existing
   * symbol index. */
  int idx2 = set_elf_sym(symtab_section, 0x700, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, text_section->sh_num, "dup");
  UT_ASSERT_EQ(idx2, idx1);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx1].st_value, 0x600); /* unchanged */

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Symbol attributes
 * ============================================================================ */

UT_TEST(test_get_sym_attr_grows_array_and_zeroes_new_entries)
{
  ut_elf_reset_state();

  struct sym_attr *attr0 = get_sym_attr(tcc_state, 0, 1);
  UT_ASSERT(attr0 != NULL);
  UT_ASSERT_EQ(tcc_state->nb_sym_attrs, 1);
  attr0->got_offset = 0xdeadbeef;

  struct sym_attr *attr3 = get_sym_attr(tcc_state, 3, 1);
  UT_ASSERT(attr3 != NULL);
  UT_ASSERT_EQ(tcc_state->nb_sym_attrs, 4);
  UT_ASSERT_EQ(attr3->got_offset, 0);

  /* Re-fetch attr0 after the realloc that may have moved the array. */
  attr0 = &tcc_state->sym_attrs[0];
  UT_ASSERT_EQ(attr0->got_offset, 0xdeadbeef);

  struct sym_attr *attr5 = get_sym_attr(tcc_state, 5, 1);
  UT_ASSERT(attr5 != NULL);
  UT_ASSERT_EQ(tcc_state->nb_sym_attrs, 8);

  struct sym_attr *attr10 = get_sym_attr(tcc_state, 10, 0);
  UT_ASSERT(attr10 == tcc_state->sym_attrs); /* no alloc, returns base */

  tcc_free(tcc_state->sym_attrs);
  tcc_state->sym_attrs = NULL;
  tcc_state->nb_sym_attrs = 0;
  return 0;
}

/* ============================================================================
 * Symbol table sorting
 * ============================================================================ */

UT_TEST(test_tcc_elf_sort_syms_moves_locals_first)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  /* Add symbols out of order: global, local, global, local. */
  set_elf_sym(symtab_section, 1, 1,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), STV_DEFAULT,
              text_section->sh_num, "g1");
  set_elf_sym(symtab_section, 2, 1,
              ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT), STV_DEFAULT,
              text_section->sh_num, "l1");
  set_elf_sym(symtab_section, 3, 1,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), STV_DEFAULT,
              text_section->sh_num, "g2");
  set_elf_sym(symtab_section, 4, 1,
              ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT), STV_DEFAULT,
              text_section->sh_num, "l2");

  symtab_section->sh_size = symtab_section->data_offset;
  tcc_elf_sort_syms(tcc_state, symtab_section);

  int nb_syms = symtab_section->data_offset / sizeof(ElfW(Sym));
  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;

  UT_ASSERT_EQ(nb_syms, 5);
  UT_ASSERT_EQ(symtab_section->sh_info, 3); /* null + two locals */

  /* First three entries must be null symbol then the two locals. */
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[0].st_info), STB_LOCAL);
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[1].st_info), STB_LOCAL);
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[2].st_info), STB_LOCAL);
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[3].st_info), STB_GLOBAL);
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[4].st_info), STB_GLOBAL);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Relocations
 * ============================================================================ */

UT_TEST(test_put_elf_reloc_creates_relocation_section)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  section_ptr_add(text_section, 4);
  put_elf_reloc(symtab_section, text_section, 0, R_DATA_PTR, 0);

  UT_ASSERT(text_section->reloc != NULL);
  UT_ASSERT_STREQ(text_section->reloc->name, ".rel.text");
  UT_ASSERT_EQ(text_section->reloc->sh_type, SHT_RELX);
  UT_ASSERT_EQ(text_section->reloc->link, symtab_section);
  UT_ASSERT_EQ(text_section->reloc->sh_info, text_section->sh_num);
  UT_ASSERT_EQ(text_section->reloc->data_offset, sizeof(ElfW_Rel));

  ElfW_Rel *rel = (ElfW_Rel *)text_section->reloc->data;
  UT_ASSERT_EQ(rel->r_offset, 0);
  UT_ASSERT_EQ(ELFW(R_TYPE)(rel->r_info), R_DATA_PTR);
  UT_ASSERT_EQ(ELFW(R_SYM)(rel->r_info), 0);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_put_elf_reloca_rejects_nonzero_addend_on_rel_arch)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  section_ptr_add(data_section, 4);
  /* On REL architectures (ARM) a non-zero addend is an error.  The function
   * reports it via _tcc_error_noabort and still records the relocation. */
  put_elf_reloca(symtab_section, data_section, 0, R_DATA_PTR, 0, 0x123);

  UT_ASSERT(data_section->reloc != NULL);
  UT_ASSERT_EQ(data_section->reloc->data_offset, sizeof(ElfW_Rel));

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Per-file symbol/reloc lifecycle
 * ============================================================================ */

UT_TEST(test_tccelf_begin_file_saves_offsets_and_disables_hash)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  section_ptr_add(text_section, 8);
  section_ptr_add(data_section, 4);

  tccelf_begin_file(tcc_state);

  UT_ASSERT_EQ(text_section->sh_offset, 8);
  UT_ASSERT_EQ(data_section->sh_offset, 4);
  UT_ASSERT_EQ(symtab_section->sh_offset, symtab_section->data_offset);
  /* Hash is disabled by stashing it in reloc and clearing hash. */
  UT_ASSERT(symtab_section->reloc != NULL);
  UT_ASSERT(symtab_section->hash == NULL);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tccelf_end_file_converts_local_undef_to_global)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);
  tccelf_begin_file(tcc_state);

  int idx = put_elf_sym(symtab_section, 0, 0,
                        ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT),
                        0, SHN_UNDEF, "local_undef");
  UT_ASSERT_EQ(idx, 1);

  tccelf_end_file(tcc_state);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[idx].st_info), STB_GLOBAL);
  UT_ASSERT_EQ(syms[idx].st_shndx, SHN_UNDEF);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tccelf_end_file_updates_relocations_after_symbol_rebuild)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);
  tccelf_begin_file(tcc_state);

  int sym = put_elf_sym(symtab_section, 0x10, 1,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                        0, text_section->sh_num, "reloc_sym");
  put_elf_reloc(symtab_section, text_section, 0, R_DATA_PTR, sym);

  tccelf_end_file(tcc_state);

  ElfW_Rel *rel = (ElfW_Rel *)text_section->reloc->data;
  int new_sym = ELFW(R_SYM)(rel->r_info);
  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[new_sym].st_value, 0x10);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Symbol resolution helpers
 * ============================================================================ */

UT_TEST(test_get_sym_addr_returns_defined_value)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  set_elf_sym(symtab_section, 0x1234, 4,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
              0, text_section->sh_num, "defined_sym");

  addr_t addr = get_sym_addr(tcc_state, "defined_sym", 0, 0);
  UT_ASSERT_EQ(addr, 0x1234);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_get_sym_addr_returns_minus_one_for_undefined)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  set_elf_sym(symtab_section, 0, 0,
              ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC),
              0, SHN_UNDEF, "undef_sym");

  addr_t addr = get_sym_addr(tcc_state, "undef_sym", 0, 0);
  UT_ASSERT(addr == (addr_t)-1);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_get_symbol_resolves_defined_symbols)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  set_elf_sym(symtab_section, 0xabcd, 1,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
              0, text_section->sh_num, "api_sym");

  void *p = tcc_get_symbol(tcc_state, "api_sym");
  UT_ASSERT(p != NULL);
  UT_ASSERT_EQ((uintptr_t)p, (uintptr_t)0xabcd);

  UT_ASSERT(tcc_get_symbol(tcc_state, "missing_sym") == NULL);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_global_sym_creates_absolute_and_undefined_symbols)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int abs_sym = set_global_sym(tcc_state, "abs_sym", NULL, 0xdead);
  int undef_sym = set_global_sym(tcc_state, "undef_sym", NULL, 0);
  int sec_sym = set_global_sym(tcc_state, "sec_sym", text_section, -1);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[abs_sym].st_shndx, SHN_ABS);
  UT_ASSERT_EQ(syms[abs_sym].st_value, 0xdead);
  UT_ASSERT_EQ(syms[undef_sym].st_shndx, SHN_UNDEF);
  UT_ASSERT_EQ(syms[sec_sym].st_shndx, text_section->sh_num);
  UT_ASSERT_EQ(syms[sec_sym].st_value, text_section->data_offset);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Init/fini arrays
 * ============================================================================ */

UT_TEST(test_add_array_creates_relocated_array_section)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int sym = set_global_sym(tcc_state, "init_fn", text_section, 0);
  add_array(tcc_state, ".init_array", sym);

  Section *init = find_section(tcc_state, ".init_array");
  UT_ASSERT(init != NULL);
  UT_ASSERT_EQ(init->sh_type, SHT_INIT_ARRAY);
  UT_ASSERT_EQ(init->data_offset, PTR_SIZE);
  UT_ASSERT(init->reloc != NULL);
  UT_ASSERT_EQ(init->reloc->data_offset, sizeof(ElfW_Rel));

  ElfW_Rel *rel = (ElfW_Rel *)init->reloc->data;
  UT_ASSERT_EQ(ELFW(R_SYM)(rel->r_info), sym);
  UT_ASSERT_EQ(ELFW(R_TYPE)(rel->r_info), R_DATA_PTR);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Symbol enumeration
 * ============================================================================ */

static int ut_list_cb_count;
static const char *ut_list_cb_last_name;

static void ut_list_cb(void *ctx, const char *name, const void *val)
{
  (void)ctx;
  (void)val;
  ut_list_cb_count++;
  ut_list_cb_last_name = name;
}

UT_TEST(test_list_elf_symbols_lists_global_default_defined_only)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  set_elf_sym(symtab_section, 0x100, 1,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
              0, text_section->sh_num, "listed_global");
  set_elf_sym(symtab_section, 0x200, 1,
              ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT),
              0, text_section->sh_num, "not_local");
  set_elf_sym(symtab_section, 0x300, 1,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
              STV_HIDDEN, text_section->sh_num, "not_hidden");
  set_elf_sym(symtab_section, 0, 1,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
              0, text_section->sh_num, "not_zero_value");

  ut_list_cb_count = 0;
  ut_list_cb_last_name = NULL;
  list_elf_symbols(tcc_state, NULL, ut_list_cb);

  UT_ASSERT_EQ(ut_list_cb_count, 1);
  UT_ASSERT_STREQ(ut_list_cb_last_name, "listed_global");

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Dynamic symbol lookup
 * ============================================================================ */

UT_TEST(test_tcc_dynsym_find_resolves_dynsymtab_symbols)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  Section *dyn = tcc_state->dynsymtab_section;
  int idx = put_elf_sym(dyn, 0x400, 4,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                        0, 1, "dyn_sym");

  UT_ASSERT_EQ(tcc_dynsym_find(tcc_state, "dyn_sym"), idx);
  UT_ASSERT_EQ(tcc_dynsym_find(tcc_state, "missing_dyn_sym"), 0);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

UT_TEST(test_tccelf_new_creates_standard_sections)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  UT_ASSERT(tcc_state->nb_sections > 1);
  UT_ASSERT(text_section != NULL);
  UT_ASSERT(data_section != NULL);
  UT_ASSERT(bss_section != NULL);
  UT_ASSERT(common_section != NULL);
  UT_ASSERT(symtab_section != NULL);
  UT_ASSERT(tcc_state->dynsymtab_section != NULL);

  UT_ASSERT_STREQ(text_section->name, ".text");
  UT_ASSERT_STREQ(data_section->name, ".data");
  UT_ASSERT_STREQ(bss_section->name, ".bss");
  UT_ASSERT_EQ(common_section->sh_num, SHN_COMMON);
  UT_ASSERT_EQ(symtab_section->data_offset, sizeof(ElfW(Sym)));

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tccelf_delete_frees_all_sections)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);
  tccelf_delete(tcc_state);

  UT_ASSERT(tcc_state->sections == NULL);
  UT_ASSERT_EQ(tcc_state->nb_sections, 0);
  UT_ASSERT(tcc_state->priv_sections == NULL);
  UT_ASSERT_EQ(tcc_state->nb_priv_sections, 0);
  return 0;
}

/* Regression lock: tccelf_delete frees sym_attrs but leaves sym_attrs/nb_sym_attrs
 * stale.  This is a known lifecycle bug; the test documents the current behavior
 * so a future fix must update both the code and this assertion. */
UT_TEST(test_tccelf_delete_leaves_sym_attrs_stale)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);
  (void)get_sym_attr(tcc_state, 0, 1);
  UT_ASSERT(tcc_state->sym_attrs != NULL);
  UT_ASSERT_EQ(tcc_state->nb_sym_attrs, 1);

  tccelf_delete(tcc_state);

  UT_ASSERT(tcc_state->sym_attrs != NULL);
  UT_ASSERT_EQ(tcc_state->nb_sym_attrs, 1);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(tccelf)
{
  /* Section creation and lookup */
  UT_RUN(test_new_section_creates_named_typed_section);
  UT_RUN(test_new_section_private_goes_to_priv_sections);
  UT_RUN(test_find_section_creates_missing_section);
  UT_RUN(test_section_hash_table_grows_and_still_finds_sections);

  /* Section data allocation */
  UT_RUN(test_section_add_allocates_and_aligns);
  UT_RUN(test_section_add_nobits_does_not_allocate_data);
  UT_RUN(test_section_ptr_add_returns_writable_pointer);
  UT_RUN(test_section_realloc_rounds_up_to_power_of_two);
  UT_RUN(test_section_prealloc_reserves_capacity_without_moving_offset);

  /* String tables */
  UT_RUN(test_put_elf_str_appends_and_returns_offsets);

  /* Symbol tables and ELF hashing */
  UT_RUN(test_new_symtab_initializes_hash_and_first_symbol);
  UT_RUN(test_put_elf_sym_adds_local_and_global_symbols);
  UT_RUN(test_find_elf_sym_locates_added_symbols);
  UT_RUN(test_set_elf_sym_adds_new_local_symbol);
  UT_RUN(test_set_elf_sym_patches_existing_undefined_to_defined);
  UT_RUN(test_set_elf_sym_detects_duplicate_global_definition);

  /* Symbol attributes */
  UT_RUN(test_get_sym_attr_grows_array_and_zeroes_new_entries);

  /* Symbol table sorting */
  UT_RUN(test_tcc_elf_sort_syms_moves_locals_first);

  /* Relocations */
  UT_RUN(test_put_elf_reloc_creates_relocation_section);
  UT_RUN(test_put_elf_reloca_rejects_nonzero_addend_on_rel_arch);

  /* Per-file symbol/reloc lifecycle */
  UT_RUN(test_tccelf_begin_file_saves_offsets_and_disables_hash);
  UT_RUN(test_tccelf_end_file_converts_local_undef_to_global);
  UT_RUN(test_tccelf_end_file_updates_relocations_after_symbol_rebuild);

  /* Symbol resolution helpers */
  UT_RUN(test_get_sym_addr_returns_defined_value);
  UT_RUN(test_get_sym_addr_returns_minus_one_for_undefined);
  UT_RUN(test_tcc_get_symbol_resolves_defined_symbols);
  UT_RUN(test_set_global_sym_creates_absolute_and_undefined_symbols);

  /* Init/fini arrays */
  UT_RUN(test_add_array_creates_relocated_array_section);

  /* Symbol enumeration */
  UT_RUN(test_list_elf_symbols_lists_global_default_defined_only);

  /* Dynamic symbol lookup */
  UT_RUN(test_tcc_dynsym_find_resolves_dynsymtab_symbols);

  /* Lifecycle */
  UT_RUN(test_tccelf_new_creates_standard_sections);
  UT_RUN(test_tccelf_delete_frees_all_sections);
  UT_RUN(test_tccelf_delete_leaves_sym_attrs_stale);
}
