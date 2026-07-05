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

#include <fcntl.h>
#include <unistd.h>

/* These constants are private to tccelf.c; mirror them here for the tests. */
#ifndef SHF_PRIVATE
#define SHF_PRIVATE 0x80000000
#endif
#ifndef SYMTAB_INITIAL_HASH_BUCKETS
#define SYMTAB_INITIAL_HASH_BUCKETS 512
#endif

/* These ST_FUNC helpers are defined in tccelf.c but not exposed in tcc.h. */
ST_FUNC void section_ensure_loaded(TCCState *s1, Section *sec);
ST_FUNC void fill_got_entry(TCCState *s1, ElfW_Rel *rel);
ST_FUNC void ld_export_standard_symbols(TCCState *s1);
ST_FUNC ssize_t full_read(int fd, void *buf, size_t count);
ST_FUNC int tcc_object_type(int fd, ElfW(Ehdr) *h);
ST_FUNC void relocate_syms(TCCState *s1, Section *symtab, int do_resolve);
ST_FUNC int tcc_load_object_file_lazy(TCCState *s1, int fd, unsigned long file_offset);
ST_FUNC void tcc_free_lazy_objfiles(TCCState *s1);
ST_FUNC void tcc_gc_mark_phase(TCCState *s1);
ST_FUNC void tcc_load_referenced_sections(TCCState *s1);
ST_FUNC void relocate_sections(TCCState *s1);
ST_FUNC int tcc_load_object_file(TCCState *s1, int fd, unsigned long file_offset);

/* Stub for the target-specific relocation routine (lives in arm-link.c in the
 * full build).  The unit-test binary links only tccelf.c, so we provide a
 * recording no-op here to exercise relocate_section/relocate_sections. */
static int ut_reloc_call_count;
static addr_t ut_reloc_last_tgt;

ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr, addr_t addr, addr_t tgt)
{
  (void)s1;
  (void)rel;
  (void)type;
  (void)ptr;
  (void)addr;
  ut_reloc_call_count++;
  ut_reloc_last_tgt = tgt;
}

static void ut_elf_reset_state(void)
{
  memset(tcc_state, 0, sizeof(TCCState));
}

static void ut_elf_init_minimal(void)
{
  /* tccelf.c assumes sections[0] is a NULL dummy. */
  dynarray_add(&tcc_state->sections, &tcc_state->nb_sections, NULL);
}

static void ut_write32le(unsigned char *p, uint32_t v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = (v >> 24) & 0xff;
}

/* Create a temporary file in the current directory containing `len` bytes from
 * `data`.  The generated path (including the caller-supplied prefix) is written
 * to `path_out`.  Returns 0 on success, -1 on failure. */
static int ut_make_temp_file(const char *prefix, const unsigned char *data, size_t len, char *path_out,
                             size_t path_size)
{
  int fd;
  snprintf(path_out, path_size, "%sXXXXXX", prefix);
  fd = mkstemp(path_out);
  if (fd < 0)
    return -1;
  if (len > 0 && (size_t)write(fd, data, len) != len)
  {
    close(fd);
    unlink(path_out);
    return -1;
  }
  close(fd);
  return 0;
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

UT_TEST(test_section_add_updates_sh_addralign)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".align", SHT_PROGBITS, SHF_ALLOC);
  UT_ASSERT_EQ(sec->sh_addralign, 8);

  section_add(sec, 4, 16);
  UT_ASSERT_EQ(sec->sh_addralign, 16);

  section_add(sec, 4, 4);
  UT_ASSERT_EQ(sec->sh_addralign, 16); /* larger value is kept */

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

UT_TEST(test_put_elf_reloca_skips_invalid_symbol_index)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  section_ptr_add(data_section, 4);
  /* Symbol index 9999 is way past the end of the symbol table. */
  put_elf_reloca(symtab_section, data_section, 0, R_DATA_PTR, 9999, 0);

  /* The relocation is silently skipped. */
  UT_ASSERT(data_section->reloc == NULL);

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

UT_TEST(test_tccelf_end_file_sets_undef_func_to_notype_for_obj_output)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);
  tccelf_begin_file(tcc_state);

  tcc_state->output_type = TCC_OUTPUT_OBJ;

  put_elf_sym(symtab_section, 0, 0,
              ELFW(ST_INFO)(STB_LOCAL, STT_FUNC),
              0, SHN_UNDEF, "local_undef_func");

  tccelf_end_file(tcc_state);

  int new_idx = find_elf_sym(symtab_section, "local_undef_func");
  UT_ASSERT(new_idx != 0);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[new_idx].st_info), STB_GLOBAL);
  UT_ASSERT_EQ(ELFW(ST_TYPE)(syms[new_idx].st_info), STT_NOTYPE);

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
UT_TEST(test_tccelf_delete_resets_sym_attrs)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);
  (void)get_sym_attr(tcc_state, 0, 1);
  UT_ASSERT(tcc_state->sym_attrs != NULL);
  UT_ASSERT_EQ(tcc_state->nb_sym_attrs, 1);

  tccelf_delete(tcc_state);

  /* tccelf_delete() resets sym_attrs/nb_sym_attrs so a reused TCCState does
   * not read through a freed allocation. */
  UT_ASSERT(tcc_state->sym_attrs == NULL);
  UT_ASSERT_EQ(tcc_state->nb_sym_attrs, 0);
  return 0;
}

/* ============================================================================
 * tccelf_new optional branches
 * ============================================================================ */

UT_TEST(test_tccelf_new_creates_bounds_sections_when_enabled)
{
  ut_elf_reset_state();
  tcc_state->do_bounds_check = 1;
  tccelf_new(tcc_state);

  UT_ASSERT(bounds_section != NULL);
  UT_ASSERT(lbounds_section != NULL);
  UT_ASSERT_STREQ(bounds_section->name, ".bounds");
  UT_ASSERT_STREQ(lbounds_section->name, ".lbounds");

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tccelf_new_calls_debug_new_when_enabled)
{
  ut_elf_reset_state();
  tcc_state->do_debug = 1;
  tccelf_new(tcc_state);
  /* tcc_debug_new is a stub in this binary; just verify no crash. */
  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Section type-specific alignment
 * ============================================================================ */

UT_TEST(test_new_section_sets_sh_addralign_by_type)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *strtab = new_section(tcc_state, ".strtab", SHT_STRTAB, SHF_PRIVATE);
  Section *hash = new_section(tcc_state, ".hashtab", SHT_HASH, SHF_PRIVATE);
  Section *gnu_hash = new_section(tcc_state, ".gnu.hash", SHT_GNU_HASH, SHF_ALLOC);
  Section *versym = new_section(tcc_state, ".gnu.version", SHT_GNU_versym, SHF_ALLOC);

  UT_ASSERT_EQ(strtab->sh_addralign, 1);
  UT_ASSERT_EQ(hash->sh_addralign, 8);
  UT_ASSERT_EQ(gnu_hash->sh_addralign, 8);
  UT_ASSERT_EQ(versym->sh_addralign, 2);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * String tables
 * ============================================================================ */

UT_TEST(test_put_elf_str_appends_duplicates)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *strtab = new_section(tcc_state, ".strtab", SHT_STRTAB, SHF_PRIVATE);
  int off1 = put_elf_str(strtab, "duplicate");
  int off2 = put_elf_str(strtab, "duplicate");

  /* put_elf_str does not deduplicate. */
  UT_ASSERT_EQ(off1, 0);
  UT_ASSERT_EQ(off2, 10);
  UT_ASSERT_STREQ((char *)strtab->data + off1, "duplicate");
  UT_ASSERT_STREQ((char *)strtab->data + off2, "duplicate");

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Symbol tables and ELF hashing
 * ============================================================================ */

UT_TEST(test_put_elf_sym_rejects_invalid_first_byte)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *symtab = new_symtab(tcc_state, ".symtab", SHT_SYMTAB, 0,
                               ".strtab", ".hashtab", SHF_PRIVATE);

  const char ctrl[] = {0x01, 'c', 't', 'r', 'l', 0};
  const char cont[] = {0x80, 'c', 'o', 'n', 't', 0};
  const char too_high[] = {0xff, 'h', 'i', 'g', 'h', 0};
  const char valid_utf8[] = {0xc2, 0xa0, 'v', 'a', 'l', 'i', 'd', 0};

  int i_ctrl = put_elf_sym(symtab, 0, 1,
                           ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), 0, 1, ctrl);
  int i_cont = put_elf_sym(symtab, 0, 1,
                           ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), 0, 1, cont);
  int i_high = put_elf_sym(symtab, 0, 1,
                           ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), 0, 1, too_high);
  int i_valid = put_elf_sym(symtab, 0, 1,
                            ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), 0, 1, valid_utf8);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab->data;
  UT_ASSERT_EQ(syms[i_ctrl].st_name, 0);
  UT_ASSERT_EQ(syms[i_cont].st_name, 0);
  UT_ASSERT_EQ(syms[i_high].st_name, 0);
  UT_ASSERT(syms[i_valid].st_name != 0);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_put_elf_sym_hash_table_rebuilds_after_many_globals)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *symtab = new_symtab(tcc_state, ".symtab", SHT_SYMTAB, 0,
                               ".strtab", ".hashtab", SHF_PRIVATE);

  char name[32];
  int i;
  for (i = 0; i < 1100; i++)
  {
    snprintf(name, sizeof(name), "sym_%04d", i);
    put_elf_sym(symtab, i, 1, ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), 0, 1, name);
  }

  /* Hash should have been resized from 512 buckets to at least 1024. */
  int *hash = (int *)symtab->hash->data;
  UT_ASSERT(hash[0] >= 1024);

  /* Every symbol must still be findable. */
  for (i = 0; i < 1100; i++)
  {
    snprintf(name, sizeof(name), "sym_%04d", i);
    int idx = find_elf_sym(symtab, name);
    UT_ASSERT(idx != 0);
    ElfW(Sym) *syms = (ElfW(Sym) *)symtab->data;
    UT_ASSERT_EQ(syms[idx].st_value, (addr_t)i);
  }

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_put_elf_sym_tracks_undefined_globals)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  UT_ASSERT_EQ(tcc_state->nb_undef_syms, 0);
  put_elf_sym(symtab_section, 0, 0,
              ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC), 0, SHN_UNDEF, "undef1");
  UT_ASSERT_EQ(tcc_state->nb_undef_syms, 1);
  put_elf_sym(symtab_section, 0, 0,
              ELFW(ST_INFO)(STB_LOCAL, STT_FUNC), 0, SHN_UNDEF, "undef_local");
  UT_ASSERT_EQ(tcc_state->nb_undef_syms, 1); /* locals not tracked */

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * set_elf_sym duplicate-definition policy branches
 * ============================================================================ */

UT_TEST(test_set_elf_sym_identical_redefinition_returns_same_index)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx1 = set_elf_sym(symtab_section, 0x100, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, text_section->sh_num, "same");
  int idx2 = set_elf_sym(symtab_section, 0x100, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, text_section->sh_num, "same");
  UT_ASSERT_EQ(idx1, idx2);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_global_overrides_weak)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx_weak = set_elf_sym(symtab_section, 0x100, 4,
                             ELFW(ST_INFO)(STB_WEAK, STT_OBJECT),
                             STV_DEFAULT, text_section->sh_num, "weakglobal");
  int idx_global = set_elf_sym(symtab_section, 0x200, 8,
                               ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                               STV_DEFAULT, text_section->sh_num, "weakglobal");
  UT_ASSERT_EQ(idx_weak, idx_global);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx_global].st_value, 0x200);
  UT_ASSERT_EQ(syms[idx_global].st_size, 8);
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[idx_global].st_info), STB_GLOBAL);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_weak_ignored_when_global_exists)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx_global = set_elf_sym(symtab_section, 0x300, 4,
                               ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                               STV_DEFAULT, text_section->sh_num, "globalweak");
  int idx_weak = set_elf_sym(symtab_section, 0x400, 8,
                             ELFW(ST_INFO)(STB_WEAK, STT_OBJECT),
                             STV_DEFAULT, text_section->sh_num, "globalweak");
  UT_ASSERT_EQ(idx_global, idx_weak);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx_global].st_value, 0x300);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_first_weak_kept)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx1 = set_elf_sym(symtab_section, 0x500, 4,
                         ELFW(ST_INFO)(STB_WEAK, STT_OBJECT),
                         STV_DEFAULT, text_section->sh_num, "weakweak");
  int idx2 = set_elf_sym(symtab_section, 0x600, 4,
                         ELFW(ST_INFO)(STB_WEAK, STT_OBJECT),
                         STV_DEFAULT, text_section->sh_num, "weakweak");
  UT_ASSERT_EQ(idx1, idx2);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx1].st_value, 0x500);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_hidden_ignored_after_defined)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx1 = set_elf_sym(symtab_section, 0x700, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, text_section->sh_num, "hideme");
  int idx2 = set_elf_sym(symtab_section, 0x800, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_HIDDEN, text_section->sh_num, "hideme");
  UT_ASSERT_EQ(idx1, idx2);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx1].st_value, 0x700);
  /* Visibility is still propagated to the most constraining value. */
  UT_ASSERT_EQ(ELFW(ST_VISIBILITY)(syms[idx1].st_other), STV_HIDDEN);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_data_takes_precedence_over_bss)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx1 = set_elf_sym(symtab_section, 0, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, bss_section->sh_num, "databss");
  int idx2 = set_elf_sym(symtab_section, 0x900, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, data_section->sh_num, "databss");
  UT_ASSERT_EQ(idx1, idx2);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx1].st_value, 0x900);
  UT_ASSERT_EQ(syms[idx1].st_shndx, data_section->sh_num);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_data_keeps_precedence_over_common)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx1 = set_elf_sym(symtab_section, 0xa00, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, data_section->sh_num, "datacommon");
  int idx2 = set_elf_sym(symtab_section, 0, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, SHN_COMMON, "datacommon");
  UT_ASSERT_EQ(idx1, idx2);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx1].st_value, 0xa00);
  UT_ASSERT_EQ(syms[idx1].st_shndx, data_section->sh_num);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_common_to_data_takes_precedence)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx1 = set_elf_sym(symtab_section, 0, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, SHN_COMMON, "commontodata");
  int idx2 = set_elf_sym(symtab_section, 0xb00, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, data_section->sh_num, "commontodata");
  UT_ASSERT_EQ(idx1, idx2);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx1].st_value, 0xb00);
  UT_ASSERT_EQ(syms[idx1].st_shndx, data_section->sh_num);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_visibility_propagation_weak_to_global)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx_weak = set_elf_sym(symtab_section, 0xc00, 4,
                             ELFW(ST_INFO)(STB_WEAK, STT_OBJECT),
                             STV_DEFAULT, text_section->sh_num, "visprop");
  int idx_global = set_elf_sym(symtab_section, 0xd00, 4,
                               ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                               STV_PROTECTED, text_section->sh_num, "visprop");
  UT_ASSERT_EQ(idx_weak, idx_global);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx_global].st_value, 0xd00);
  UT_ASSERT_EQ(ELFW(ST_VISIBILITY)(syms[idx_global].st_other), STV_PROTECTED);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_visibility_default_after_nondefault)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx_weak = set_elf_sym(symtab_section, 0xc10, 4,
                             ELFW(ST_INFO)(STB_WEAK, STT_OBJECT),
                             STV_PROTECTED, text_section->sh_num, "visprop2");
  int idx_global = set_elf_sym(symtab_section, 0xd10, 4,
                               ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                               STV_DEFAULT, text_section->sh_num, "visprop2");
  UT_ASSERT_EQ(idx_weak, idx_global);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx_global].st_value, 0xd10);
  UT_ASSERT_EQ(ELFW(ST_VISIBILITY)(syms[idx_global].st_other), STV_PROTECTED);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_visibility_both_nondefault)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx_weak = set_elf_sym(symtab_section, 0xc20, 4,
                             ELFW(ST_INFO)(STB_WEAK, STT_OBJECT),
                             STV_PROTECTED, text_section->sh_num, "visprop3");
  int idx_global = set_elf_sym(symtab_section, 0xd20, 4,
                               ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                               STV_HIDDEN, text_section->sh_num, "visprop3");
  UT_ASSERT_EQ(idx_weak, idx_global);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx_global].st_value, 0xd20);
  UT_ASSERT_EQ(ELFW(ST_VISIBILITY)(syms[idx_global].st_other), STV_HIDDEN);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_elf_sym_asm_set_overridden)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx1 = set_elf_sym(symtab_section, 0xe00, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT | ST_ASM_SET, text_section->sh_num, "asmset");
  int idx2 = set_elf_sym(symtab_section, 0xf00, 4,
                         ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                         STV_DEFAULT, text_section->sh_num, "asmset");
  UT_ASSERT_EQ(idx1, idx2);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx1].st_value, 0xf00);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Symbol resolution helpers
 * ============================================================================ */

UT_TEST(test_get_sym_addr_err_reports_undefined)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  addr_t addr = get_sym_addr(tcc_state, "no_such_symbol", 1, 0);
  UT_ASSERT(addr == (addr_t)-1);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_get_sym_addr_with_leading_underscore)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);
  tcc_state->leading_underscore = 1;

  set_elf_sym(symtab_section, 0x12345678, 4,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
              0, text_section->sh_num, "_underscored");

  addr_t addr = get_sym_addr(tcc_state, "underscored", 0, 1);
  UT_ASSERT_EQ(addr, 0x12345678);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_list_symbols_wrapper_lists_symbols)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  set_elf_sym(symtab_section, 0x777, 1,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
              0, text_section->sh_num, "wrapper_sym");

  ut_list_cb_count = 0;
  ut_list_cb_last_name = NULL;
  tcc_list_symbols(tcc_state, NULL, ut_list_cb);
  UT_ASSERT_EQ(ut_list_cb_count, 1);
  UT_ASSERT_STREQ(ut_list_cb_last_name, "wrapper_sym");

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_set_global_sym_null_name_creates_local_absolute)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx = set_global_sym(tcc_state, NULL, NULL, 0xabc);
  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(ELFW(ST_BIND)(syms[idx].st_info), STB_LOCAL);
  UT_ASSERT_EQ(syms[idx].st_shndx, SHN_ABS);
  UT_ASSERT_EQ(syms[idx].st_value, 0xabc);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Bound checking helper
 * ============================================================================ */

UT_TEST(test_tcc_add_bcheck_noop_when_bounds_disabled)
{
  ut_elf_reset_state();
  tcc_state->do_bounds_check = 0;
  tccelf_new(tcc_state);

  /* With bounds checking disabled, .bounds is not created; the helper
   * simply returns.  Just verify it does not crash or touch state. */
  tcc_add_bcheck(tcc_state);
  UT_ASSERT(bounds_section == NULL);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_add_bcheck_adds_when_bounds_enabled)
{
  ut_elf_reset_state();
  tcc_state->do_bounds_check = 1;
  tccelf_new(tcc_state);

  size_t before = bounds_section->data_offset;
  tcc_add_bcheck(tcc_state);
  UT_ASSERT_EQ(bounds_section->data_offset, before + sizeof(addr_t));

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Lazy section materialization
 * ============================================================================ */

UT_TEST(test_section_materialize_loads_deferred_chunk)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".lazysec", SHT_PROGBITS, SHF_ALLOC);
  unsigned char payload[] = {0x11, 0x22, 0x33, 0x44};
  char path[64];

  UT_ASSERT(ut_make_temp_file("tccelf_ut_mat_", payload, sizeof(payload), path, sizeof(path)) == 0);

  DeferredChunk *chunk = (DeferredChunk *)tcc_mallocz(sizeof(DeferredChunk));
  chunk->source_path = tcc_strdup(path);
  chunk->file_offset = 0;
  chunk->size = sizeof(payload);
  chunk->dest_offset = 0;
  sec->deferred_head = chunk;
  sec->deferred_tail = chunk;
  sec->lazy = 1;
  sec->has_deferred_chunks = 1;
  sec->data_offset = sizeof(payload);

  section_materialize(tcc_state, sec);

  UT_ASSERT(sec->materialized);
  UT_ASSERT(!sec->lazy);
  UT_ASSERT(memcmp(sec->data, payload, sizeof(payload)) == 0);

  unlink(path);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_materialize_honors_deferred_dest_offset)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".lazyoffset", SHT_PROGBITS, SHF_ALLOC);
  unsigned char payload[] = {0xaa, 0xbb, 0xcc, 0xdd};
  char path[64];

  UT_ASSERT(ut_make_temp_file("tccelf_ut_matoff_", payload, sizeof(payload), path, sizeof(path)) == 0);

  DeferredChunk *chunk = (DeferredChunk *)tcc_mallocz(sizeof(DeferredChunk));
  chunk->source_path = tcc_strdup(path);
  chunk->file_offset = 0;
  chunk->size = sizeof(payload);
  chunk->dest_offset = 4;
  sec->deferred_head = chunk;
  sec->deferred_tail = chunk;
  sec->lazy = 1;
  sec->has_deferred_chunks = 1;
  sec->data_offset = 8;

  section_materialize(tcc_state, sec);

  UT_ASSERT(sec->materialized);
  UT_ASSERT_EQ(read32le(sec->data), 0u);
  UT_ASSERT(memcmp(sec->data + 4, payload, sizeof(payload)) == 0);

  unlink(path);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_materialize_nobits_no_chunks_marks_done)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".lazynobits", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);
  sec->lazy = 1;
  sec->has_deferred_chunks = 0;
  sec->data_offset = 16;

  section_materialize(tcc_state, sec);

  UT_ASSERT(sec->materialized);
  UT_ASSERT(sec->data == NULL);
  UT_ASSERT_EQ(sec->data_allocated, 0);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_ensure_loaded_frees_discarded_chunks)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".discarded", SHT_PROGBITS, SHF_ALLOC);
  unsigned char dummy = 0;
  char path[64];

  UT_ASSERT(ut_make_temp_file("tccelf_ut_disc_", &dummy, 1, path, sizeof(path)) == 0);

  DeferredChunk *chunk = (DeferredChunk *)tcc_mallocz(sizeof(DeferredChunk));
  chunk->source_path = tcc_strdup(path);
  chunk->size = 1;
  sec->deferred_head = chunk;
  sec->deferred_tail = chunk;
  sec->lazy = 1;
  sec->has_deferred_chunks = 1;
  sec->data_offset = 0; /* GC'd section */

  section_ensure_loaded(tcc_state, sec);

  UT_ASSERT(!sec->lazy);
  UT_ASSERT(!sec->has_deferred_chunks);
  UT_ASSERT(sec->deferred_head == NULL);

  unlink(path);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_apply_reloc_patches_during_materialize)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".patched", SHT_PROGBITS, SHF_ALLOC);
  unsigned char zeros[8] = {0};
  char path[64];

  UT_ASSERT(ut_make_temp_file("tccelf_ut_patch_", zeros, sizeof(zeros), path, sizeof(path)) == 0);

  sec->data_offset = sizeof(zeros);
  section_realloc(sec, sec->data_offset);

  uint32_t *offsets = (uint32_t *)tcc_malloc(2 * sizeof(uint32_t));
  uint32_t *values = (uint32_t *)tcc_malloc(2 * sizeof(uint32_t));
  offsets[0] = 0;
  values[0] = 0x12345678;
  offsets[1] = 4;
  values[1] = 0xaabbccdd;
  sec->reloc_patch_offsets = offsets;
  sec->reloc_patch_values = values;
  sec->nb_reloc_patches = 2;
  sec->alloc_reloc_patches = 2;

  DeferredChunk *chunk = (DeferredChunk *)tcc_mallocz(sizeof(DeferredChunk));
  chunk->source_path = tcc_strdup(path);
  chunk->size = sizeof(zeros);
  sec->deferred_head = chunk;
  sec->deferred_tail = chunk;
  sec->lazy = 1;
  sec->has_deferred_chunks = 1;

  section_materialize(tcc_state, sec);

  UT_ASSERT_EQ(read32le(sec->data + 0), 0x12345678u);
  UT_ASSERT_EQ(read32le(sec->data + 4), 0xaabbccddu);

  unlink(path);
  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Symbol relocation
 * ============================================================================ */

UT_TEST(test_relocate_syms_adds_section_base)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  text_section->sh_addr = 0x1000;

  int idx = set_elf_sym(symtab_section, 0x10, 4,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC),
                        0, text_section->sh_num, "relocated_fn");

  relocate_syms(tcc_state, symtab_section, 0);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx].st_value, (addr_t)0x1010);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_relocate_syms_undefined_weak_zeroes)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx = set_elf_sym(symtab_section, 0, 0,
                        ELFW(ST_INFO)(STB_WEAK, STT_FUNC),
                        0, SHN_UNDEF, "weak_undef");

  relocate_syms(tcc_state, symtab_section, 0);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx].st_value, (addr_t)0);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_resolve_common_syms_allocates_in_bss)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx = set_elf_sym(symtab_section, 8, 16,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                        0, SHN_COMMON, "common_var");

  resolve_common_syms(tcc_state);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx].st_shndx, bss_section->sh_num);
  UT_ASSERT_EQ(syms[idx].st_value, (addr_t)0);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * GOT filling
 * ============================================================================ */

UT_TEST(test_fill_got_entry_writes_symbol_value)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  Section *got = new_section(tcc_state, ".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
  tcc_state->got = got;

  int idx = set_elf_sym(symtab_section, 0xdeadbeef, 4,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                        0, text_section->sh_num, "got_var");

  struct sym_attr *attr = get_sym_attr(tcc_state, idx, 1);
  attr->got_offset = 4;

  ElfW_Rel rel;
  rel.r_offset = 0;
  rel.r_info = ELFW(R_INFO)(idx, R_DATA_PTR);

  fill_got_entry(tcc_state, &rel);

  UT_ASSERT_EQ(read32le(got->data + 4), 0xdeadbeefu);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * ELF object type detection
 * ============================================================================ */

UT_TEST(test_tcc_object_type_detects_rel_elf)
{
  unsigned char ehdr[52] = {0};
  ElfW(Ehdr) h;
  char path[64];
  int fd, type;

  ehdr[0] = ELFMAG0;
  ehdr[1] = ELFMAG1;
  ehdr[2] = ELFMAG2;
  ehdr[3] = ELFMAG3;
  ehdr[4] = ELFCLASS32;
  ehdr[5] = ELFDATA2LSB;
  ehdr[6] = EV_CURRENT;
  ehdr[16] = ET_REL & 0xff;
  ehdr[17] = (ET_REL >> 8) & 0xff;
  ehdr[18] = EM_ARM & 0xff;
  ehdr[19] = (EM_ARM >> 8) & 0xff;
  ehdr[20] = EV_CURRENT & 0xff;

  UT_ASSERT(ut_make_temp_file("tccelf_ut_obj_", ehdr, sizeof(ehdr), path, sizeof(path)) == 0);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  type = tcc_object_type(fd, &h);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(type, AFF_BINTYPE_REL);
  UT_ASSERT_EQ(h.e_type, ET_REL);

  return 0;
}

UT_TEST(test_tcc_object_type_unrecognized_returns_zero)
{
  unsigned char garbage[] = "not an elf or archive";
  char path[64];
  int fd, type;
  ElfW(Ehdr) h;

  UT_ASSERT(ut_make_temp_file("tccelf_ut_bad_", garbage, sizeof(garbage), path, sizeof(path)) == 0);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  type = tcc_object_type(fd, &h);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(type, 0);

  return 0;
}

UT_TEST(test_tcc_object_type_wrong_class_rejected)
{
  unsigned char ehdr[52] = {0};
  ElfW(Ehdr) h;
  char path[64];
  int fd, type;

  ehdr[0] = ELFMAG0;
  ehdr[1] = ELFMAG1;
  ehdr[2] = ELFMAG2;
  ehdr[3] = ELFMAG3;
  ehdr[4] = ELFCLASS64; /* target is 32-bit ARM */
  ehdr[5] = ELFDATA2LSB;
  ehdr[6] = EV_CURRENT;
  ehdr[16] = ET_REL & 0xff;

  UT_ASSERT(ut_make_temp_file("tccelf_ut_class_", ehdr, sizeof(ehdr), path, sizeof(path)) == 0);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  type = tcc_object_type(fd, &h);
  close(fd);
  unlink(path);

  /* tcc_object_type() validates e_ident[EI_CLASS] against the target ELF
   * class, so an ELFCLASS64 header on this 32-bit target is not recognized. */
  UT_ASSERT_EQ(type, 0);
  return 0;
}

UT_TEST(test_full_read_loads_exact_bytes)
{
  unsigned char data[] = "hello";
  char path[64];
  int fd;
  char buf[8] = {0};
  ssize_t n;

  UT_ASSERT(ut_make_temp_file("tccelf_ut_full_", data, sizeof(data), path, sizeof(path)) == 0);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  n = full_read(fd, buf, sizeof(data));
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(n, (ssize_t)sizeof(data));
  UT_ASSERT_STREQ(buf, "hello");

  return 0;
}

UT_TEST(test_load_data_reads_from_offset)
{
  unsigned char data[] = "hello world";
  char path[64];
  int fd;
  unsigned char *p;

  UT_ASSERT(ut_make_temp_file("tccelf_ut_ld_", data, sizeof(data), path, sizeof(path)) == 0);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  p = (unsigned char *)load_data(fd, 6, 5);
  close(fd);
  unlink(path);

  UT_ASSERT(memcmp(p, "world", 5) == 0);
  tcc_free(p);

  return 0;
}

/* ============================================================================
 * Standard linker symbols
 * ============================================================================ */

UT_TEST(test_ld_export_standard_symbols_exports_section_boundaries)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  text_section->sh_addr = 0x1000;
  text_section->sh_size = 0x100;
  data_section->sh_addr = 0x2000;
  data_section->sh_size = 0x80;
  bss_section->sh_addr = 0x3000;
  bss_section->sh_size = 0x40;

  ld_export_standard_symbols(tcc_state);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  int idx_text_start = find_elf_sym(symtab_section, "__text_start__");
  int idx_text_end = find_elf_sym(symtab_section, "__text_end__");
  int idx_data_start = find_elf_sym(symtab_section, "__data_start__");
  int idx_edata = find_elf_sym(symtab_section, "_edata");
  int idx_bss_start = find_elf_sym(symtab_section, "__bss_start__");
  int idx_bss_end = find_elf_sym(symtab_section, "__bss_end__");
  int idx_end = find_elf_sym(symtab_section, "_end");

  UT_ASSERT(idx_text_start != 0);
  UT_ASSERT(idx_text_end != 0);
  UT_ASSERT(idx_data_start != 0);
  UT_ASSERT(idx_edata != 0);
  UT_ASSERT(idx_bss_start != 0);
  UT_ASSERT(idx_bss_end != 0);
  UT_ASSERT(idx_end != 0);

  UT_ASSERT_EQ(syms[idx_text_start].st_value, (addr_t)0x1000);
  UT_ASSERT_EQ(syms[idx_text_end].st_value, (addr_t)0x1100);
  UT_ASSERT_EQ(syms[idx_data_start].st_value, (addr_t)0x2000);
  UT_ASSERT_EQ(syms[idx_edata].st_value, (addr_t)0x2080);
  UT_ASSERT_EQ(syms[idx_bss_start].st_value, (addr_t)0x3000);
  UT_ASSERT_EQ(syms[idx_bss_end].st_value, (addr_t)0x3040);
  UT_ASSERT_EQ(syms[idx_end].st_value, (addr_t)0x3040);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * File helpers
 * ============================================================================ */

UT_TEST(test_full_read_returns_error_on_bad_fd)
{
  char buf[4];
  ssize_t n = full_read(-1, buf, sizeof(buf));
  UT_ASSERT(n < 0);
  return 0;
}

/* ============================================================================
 * ELF object type detection (remaining branches)
 * ============================================================================ */

UT_TEST(test_tcc_object_type_detects_dyn_elf)
{
  unsigned char ehdr[52] = {0};
  ElfW(Ehdr) h;
  char path[64];
  int fd, type;

  ehdr[0] = ELFMAG0;
  ehdr[1] = ELFMAG1;
  ehdr[2] = ELFMAG2;
  ehdr[3] = ELFMAG3;
  ehdr[4] = ELFCLASS32;
  ehdr[5] = ELFDATA2LSB;
  ehdr[6] = EV_CURRENT;
  ehdr[16] = ET_DYN & 0xff;
  ehdr[17] = (ET_DYN >> 8) & 0xff;
  ehdr[18] = EM_ARM & 0xff;
  ehdr[19] = (EM_ARM >> 8) & 0xff;

  UT_ASSERT(ut_make_temp_file("tccelf_ut_dyn_", ehdr, sizeof(ehdr), path, sizeof(path)) == 0);
  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  type = tcc_object_type(fd, &h);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(type, AFF_BINTYPE_DYN);
  UT_ASSERT_EQ(h.e_type, ET_DYN);
  return 0;
}

UT_TEST(test_tcc_object_type_detects_archive)
{
  unsigned char ar[] = "!<arch>\nrest...";
  ElfW(Ehdr) h;
  char path[64];
  int fd, type;

  UT_ASSERT(ut_make_temp_file("tccelf_ut_ar_", ar, sizeof(ar), path, sizeof(path)) == 0);
  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  type = tcc_object_type(fd, &h);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(type, AFF_BINTYPE_AR);
  return 0;
}

UT_TEST(test_tcc_object_type_detects_yaff)
{
  unsigned char yaff[] = "YAFFpayload";
  ElfW(Ehdr) h;
  char path[64];
  int fd, type;

  UT_ASSERT(ut_make_temp_file("tccelf_ut_yaff_", yaff, sizeof(yaff), path, sizeof(path)) == 0);
  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  type = tcc_object_type(fd, &h);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(type, AFF_BINTYPE_YAFF);
  return 0;
}

UT_TEST(test_tcc_object_type_exec_elf_returns_zero)
{
  unsigned char ehdr[52] = {0};
  ElfW(Ehdr) h;
  char path[64];
  int fd, type;

  ehdr[0] = ELFMAG0;
  ehdr[1] = ELFMAG1;
  ehdr[2] = ELFMAG2;
  ehdr[3] = ELFMAG3;
  ehdr[4] = ELFCLASS32;
  ehdr[5] = ELFDATA2LSB;
  ehdr[6] = EV_CURRENT;
  ehdr[16] = ET_EXEC & 0xff;
  ehdr[17] = (ET_EXEC >> 8) & 0xff;
  ehdr[18] = EM_ARM & 0xff;
  ehdr[19] = (EM_ARM >> 8) & 0xff;

  UT_ASSERT(ut_make_temp_file("tccelf_ut_exec_", ehdr, sizeof(ehdr), path, sizeof(path)) == 0);
  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  type = tcc_object_type(fd, &h);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(type, 0);
  return 0;
}

/* ============================================================================
 * Symbol relocation (remaining branches)
 * ============================================================================ */

UT_TEST(test_relocate_syms_skips_undef_when_resolving_dynsym)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  set_elf_sym(symtab_section, 0, 0,
              ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC),
              0, SHN_UNDEF, "dynskip");

  /* do_resolve == 2 is used for dynsym relocation: undefined symbols are
   * left untouched rather than reported. */
  relocate_syms(tcc_state, symtab_section, 2);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  int idx = find_elf_sym(symtab_section, "dynskip");
  UT_ASSERT(idx != 0);
  UT_ASSERT_EQ(syms[idx].st_shndx, SHN_UNDEF);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_relocate_syms_rejects_invalid_st_name)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx = put_elf_sym(symtab_section, 0, 0,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC),
                        0, SHN_UNDEF, "badname");
  /* Corrupt the string-table offset so the name validation fires. */
  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  syms[idx].st_name = (unsigned)-1;

  /* Should report an internal error and continue without crashing. */
  relocate_syms(tcc_state, symtab_section, 0);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_relocate_syms_accepts_fp_hw_undefined)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  set_elf_sym(symtab_section, 0, 0,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
              0, SHN_UNDEF, "_fp_hw");

  /* _fp_hw is accepted as undefined by ABI convention without error. */
  relocate_syms(tcc_state, symtab_section, 0);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  int idx = find_elf_sym(symtab_section, "_fp_hw");
  UT_ASSERT(idx != 0);
  UT_ASSERT_EQ(syms[idx].st_shndx, SHN_UNDEF);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_relocate_syms_reports_undefined_non_weak)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  set_elf_sym(symtab_section, 0, 0,
              ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC),
              0, SHN_UNDEF, "missing_fn");

  /* Non-weak undefined symbols are reported via tcc_error_noabort. */
  relocate_syms(tcc_state, symtab_section, 0);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Lazy section materialization (remaining branches)
 * ============================================================================ */

UT_TEST(test_section_ensure_loaded_materializes_lazy_section)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".lazyload", SHT_PROGBITS, SHF_ALLOC);
  unsigned char payload[] = {0x55, 0x66, 0x77, 0x88};
  char path[64];

  UT_ASSERT(ut_make_temp_file("tccelf_ut_ensure_", payload, sizeof(payload), path, sizeof(path)) == 0);

  DeferredChunk *chunk = (DeferredChunk *)tcc_mallocz(sizeof(DeferredChunk));
  chunk->source_path = tcc_strdup(path);
  chunk->size = sizeof(payload);
  sec->deferred_head = chunk;
  sec->deferred_tail = chunk;
  sec->lazy = 1;
  sec->has_deferred_chunks = 1;
  sec->data_offset = sizeof(payload);

  section_ensure_loaded(tcc_state, sec);

  UT_ASSERT(sec->materialized);
  UT_ASSERT(!sec->lazy);
  UT_ASSERT(memcmp(sec->data, payload, sizeof(payload)) == 0);

  unlink(path);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_materialize_skips_already_materialized)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".mat2", SHT_PROGBITS, SHF_ALLOC);
  unsigned char payload[] = {0x11, 0x22, 0x33, 0x44};
  char path[64];

  UT_ASSERT(ut_make_temp_file("tccelf_ut_mat2_", payload, sizeof(payload), path, sizeof(path)) == 0);

  DeferredChunk *chunk = (DeferredChunk *)tcc_mallocz(sizeof(DeferredChunk));
  chunk->source_path = tcc_strdup(path);
  chunk->size = sizeof(payload);
  sec->deferred_head = chunk;
  sec->deferred_tail = chunk;
  sec->lazy = 1;
  sec->has_deferred_chunks = 1;
  sec->data_offset = sizeof(payload);

  section_materialize(tcc_state, sec);
  UT_ASSERT(sec->materialized);

  /* Second call should be a no-op and not re-read or crash. */
  section_materialize(tcc_state, sec);
  UT_ASSERT(sec->materialized);
  UT_ASSERT(memcmp(sec->data, payload, sizeof(payload)) == 0);

  unlink(path);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_materialize_skips_materialized_chunk)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".mat3", SHT_PROGBITS, SHF_ALLOC);
  unsigned char payload[] = {0x11};
  char path[64];

  UT_ASSERT(ut_make_temp_file("tccelf_ut_mat3_", payload, sizeof(payload), path, sizeof(path)) == 0);

  DeferredChunk *chunk = (DeferredChunk *)tcc_mallocz(sizeof(DeferredChunk));
  chunk->source_path = tcc_strdup(path);
  chunk->size = sizeof(payload);
  chunk->materialized = 1;
  sec->deferred_head = chunk;
  sec->deferred_tail = chunk;
  sec->lazy = 1;
  sec->has_deferred_chunks = 1;
  sec->data_offset = sizeof(payload);

  section_materialize(tcc_state, sec);

  UT_ASSERT(sec->materialized);

  unlink(path);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_materialize_handles_open_failure)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".badpath", SHT_PROGBITS, SHF_ALLOC);

  DeferredChunk *chunk = (DeferredChunk *)tcc_mallocz(sizeof(DeferredChunk));
  chunk->source_path = tcc_strdup("/nonexistent/tccelf_ut_path");
  chunk->size = 4;
  sec->deferred_head = chunk;
  sec->deferred_tail = chunk;
  sec->lazy = 1;
  sec->has_deferred_chunks = 1;
  sec->data_offset = 4;

  /* Should report the error and still mark the section materialized. */
  section_materialize(tcc_state, sec);
  UT_ASSERT(sec->materialized);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_materialize_handles_short_read)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".shortread", SHT_PROGBITS, SHF_ALLOC);
  unsigned char one = 0xab;
  char path[64];

  UT_ASSERT(ut_make_temp_file("tccelf_ut_short_", &one, 1, path, sizeof(path)) == 0);

  DeferredChunk *chunk = (DeferredChunk *)tcc_mallocz(sizeof(DeferredChunk));
  chunk->source_path = tcc_strdup(path);
  chunk->size = 4; /* larger than the 1-byte file */
  sec->deferred_head = chunk;
  sec->deferred_tail = chunk;
  sec->lazy = 1;
  sec->has_deferred_chunks = 1;
  sec->data_offset = 4;

  /* Should report the short read and still finish materialization. */
  section_materialize(tcc_state, sec);
  UT_ASSERT(sec->materialized);

  unlink(path);
  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Section lifecycle
 * ============================================================================ */

UT_TEST(test_free_section_null_is_safe)
{
  free_section(NULL);
  return 0;
}

/* ============================================================================
 * Linker symbols and common-symbol resolution
 * ============================================================================ */

UT_TEST(test_ld_export_standard_symbols_updates_existing)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  text_section->sh_addr = 0x1000;
  text_section->sh_size = 0x100;
  data_section->sh_addr = 0x2000;
  data_section->sh_size = 0x80;
  bss_section->sh_addr = 0x3000;
  bss_section->sh_size = 0x40;

  /* First call creates the standard symbols. */
  ld_export_standard_symbols(tcc_state);

  /* Mutate addresses and call again; set_or_update_global_sym should update
   * the existing defined symbols instead of trying to redefine them. */
  text_section->sh_addr = 0x4000;
  text_section->sh_size = 0x200;
  data_section->sh_addr = 0x5000;
  data_section->sh_size = 0x100;
  bss_section->sh_addr = 0x6000;
  bss_section->sh_size = 0x80;

  ld_export_standard_symbols(tcc_state);

  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  int idx_text_end = find_elf_sym(symtab_section, "__text_end__");
  int idx_data_end = find_elf_sym(symtab_section, "_edata");
  int idx_bss_end = find_elf_sym(symtab_section, "__bss_end__");

  UT_ASSERT_EQ(syms[idx_text_end].st_value, (addr_t)0x4200);
  UT_ASSERT_EQ(syms[idx_data_end].st_value, (addr_t)0x5100);
  UT_ASSERT_EQ(syms[idx_bss_end].st_value, (addr_t)0x6080);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_resolve_common_syms_with_init_array_section)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  Section *init = new_section(tcc_state, ".init_array", SHT_PROGBITS, SHF_ALLOC);
  section_ptr_add(init, 8);

  set_elf_sym(symtab_section, 4, 16,
              ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
              0, SHN_COMMON, "common_var");

  resolve_common_syms(tcc_state);

  /* add_init_array_defines should use the real section and its data_offset. */
  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  int idx_start = find_elf_sym(symtab_section, "__init_array_start");
  int idx_end = find_elf_sym(symtab_section, "__init_array_end");

  UT_ASSERT(idx_start != 0);
  UT_ASSERT(idx_end != 0);
  UT_ASSERT_EQ(syms[idx_end].st_value, (addr_t)8);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_add_linker_symbols_skips_non_c_id_sections)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  /* .text.foo has a dot inside the name (after the leading dot), so it is not
   * a valid C identifier and tcc_add_linker_symbols must skip it. */
  Section *s = new_section(tcc_state, ".text.foo", SHT_PROGBITS, SHF_ALLOC);
  section_ptr_add(s, 4);

  resolve_common_syms(tcc_state);

  int idx_start = find_elf_sym(symtab_section, "__start_text_foo");
  int idx_stop = find_elf_sym(symtab_section, "__stop_text_foo");

  UT_ASSERT_EQ(idx_start, 0);
  UT_ASSERT_EQ(idx_stop, 0);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_rebuild_hash_handles_local_symbols)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  /* Add one local symbol before the global flood so rebuild_hash has to
   * process a local entry in the chain table. */
  put_elf_sym(symtab_section, 0, 1,
              ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT),
              0, text_section->sh_num, "local_first");

  char name[32];
  for (int i = 0; i < 1100; i++)
  {
    snprintf(name, sizeof(name), "rebuild_global_%04d", i);
    put_elf_sym(symtab_section, i, 1,
                ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                0, text_section->sh_num, name);
  }

  /* Every global must still be findable. */
  for (int i = 0; i < 1100; i++)
  {
    snprintf(name, sizeof(name), "rebuild_global_%04d", i);
    int idx = find_elf_sym(symtab_section, name);
    UT_ASSERT(idx != 0);
  }

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Lazy object-file loading and GC
 * ============================================================================ */

/* Build a tiny little-endian ARM ELF relocatable object with one allocated
 * PROGBITS section (.text) and one STB_GLOBAL symbol named `sym_name` bound
 * to it.  The file is written to `path_out` and its file descriptor is
 * returned open for reading. */
static int ut_make_minimal_elf_o(const char *sym_name, char *path_out, size_t path_size)
{
  unsigned char ehdr[52] = {0};
  unsigned char shdr[40 * 4] = {0}; /* 4 sections */
  unsigned char strtab[64] = {0};
  unsigned char symtab[2 * 16] = {0}; /* null + one symbol */
  unsigned char text[4] = {0x00, 0x00, 0x00, 0x00};
  int fd;

  ehdr[0] = ELFMAG0;
  ehdr[1] = ELFMAG1;
  ehdr[2] = ELFMAG2;
  ehdr[3] = ELFMAG3;
  ehdr[4] = ELFCLASS32;
  ehdr[5] = ELFDATA2LSB;
  ehdr[6] = EV_CURRENT;
  ehdr[16] = ET_REL & 0xff;
  ehdr[17] = (ET_REL >> 8) & 0xff;
  ehdr[18] = EM_ARM & 0xff;
  ehdr[19] = (EM_ARM >> 8) & 0xff;
  ehdr[20] = EV_CURRENT & 0xff;
  ehdr[32] = 52 & 0xff;        /* e_shoff */
  ehdr[33] = (52 >> 8) & 0xff;
  ehdr[34] = (52 >> 16) & 0xff;
  ehdr[35] = (52 >> 24) & 0xff;
  ehdr[40] = 52 & 0xff;        /* e_ehsize */
  ehdr[41] = (52 >> 8) & 0xff;
  /* ELF32 field offsets: e_shentsize=46, e_shnum=48, e_shstrndx=50
     (42/44 are e_phentsize/e_phnum, which stay zero). */
  ehdr[46] = 40 & 0xff;        /* e_shentsize */
  ehdr[47] = (40 >> 8) & 0xff;
  ehdr[48] = 4 & 0xff;         /* e_shnum */
  ehdr[49] = (4 >> 8) & 0xff;
  ehdr[50] = 3 & 0xff;         /* e_shstrndx */
  ehdr[51] = (3 >> 8) & 0xff;

  /* Section header 0 is null. */

  /* .text section header (index 1) */
  shdr[1 * 40 + 0] = 9;               /* sh_name offset in strtab (".text") */
  shdr[1 * 40 + 4] = SHT_PROGBITS;    /* sh_type */
  shdr[1 * 40 + 8] = SHF_ALLOC | SHF_EXECINSTR; /* sh_flags */
  shdr[1 * 40 + 16] = 52 + 40 * 4;    /* sh_offset */
  shdr[1 * 40 + 20] = sizeof(text);   /* sh_size */
  shdr[1 * 40 + 32] = 1;              /* sh_addralign */

  /* .symtab section header (index 2) */
  shdr[2 * 40 + 0] = 1;               /* sh_name offset in strtab (".symtab") */
  shdr[2 * 40 + 4] = SHT_SYMTAB;      /* sh_type */
  shdr[2 * 40 + 16] = 52 + 40 * 4 + sizeof(text); /* sh_offset */
  shdr[2 * 40 + 20] = sizeof(symtab); /* sh_size */
  shdr[2 * 40 + 24] = 3;              /* sh_link -> strtab */
  shdr[2 * 40 + 36] = 16;             /* sh_entsize */

  /* .shstrtab section header (index 3) */
  shdr[3 * 40 + 0] = 17;              /* sh_name offset */
  shdr[3 * 40 + 4] = SHT_STRTAB;      /* sh_type */
  shdr[3 * 40 + 16] = 52 + 40 * 4 + sizeof(text) + sizeof(symtab);
  shdr[3 * 40 + 20] = sizeof(strtab);

  /* String table: 0="", 1=".symtab", 9=".text", 17=".shstrtab", 28=sym_name */
  memcpy(strtab + 1, ".symtab", 8);
  memcpy(strtab + 9, ".text", 6);
  memcpy(strtab + 17, ".shstrtab", 10);
  memcpy(strtab + 28, sym_name, strlen(sym_name) + 1);

  /* Symbol table: null symbol + global defined symbol in .text */
  /* Symbol 1 at offset 16 */
  symtab[16 + 0] = 28 & 0xff;         /* st_name */
  symtab[16 + 1] = (28 >> 8) & 0xff;
  symtab[16 + 4] = 0;                 /* st_value */
  symtab[16 + 8] = sizeof(text) & 0xff; /* st_size */
  symtab[16 + 12] = ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC); /* st_info */
  symtab[16 + 14] = 1 & 0xff;         /* st_shndx */
  symtab[16 + 15] = (1 >> 8) & 0xff;

  /* Assemble the full file so the ehdr is not overwritten by later writes. */
  unsigned char file[512] = {0};
  size_t off = 0;
  memcpy(file + off, ehdr, sizeof(ehdr));
  off += sizeof(ehdr);
  memcpy(file + off, shdr, sizeof(shdr));
  off += sizeof(shdr);
  memcpy(file + off, text, sizeof(text));
  off += sizeof(text);
  memcpy(file + off, symtab, sizeof(symtab));
  off += sizeof(symtab);
  memcpy(file + off, strtab, sizeof(strtab));
  off += sizeof(strtab);

  UT_ASSERT(ut_make_temp_file("tccelf_ut_elfo_", file, off, path_out, path_size) == 0);
  fd = open(path_out, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  return fd;
}

/* Build a tiny little-endian ARM ELF relocatable object like the helper above,
 * but additionally include a .debug_info PROGBITS section so that the eager
 * object-file loader exercises the debug-section deferral paths. */
static int ut_make_minimal_elf_o_with_debug(const char *sym_name, char *path_out, size_t path_size)
{
  unsigned char ehdr[52] = {0};
  unsigned char shdr[40 * 6] = {0};
  unsigned char shstrtab[80] = {0};
  unsigned char strtab[64] = {0};
  unsigned char symtab[2 * 16] = {0};
  unsigned char text[4] = {0x11, 0x22, 0x33, 0x44};
  unsigned char debug[4] = {0x55, 0x66, 0x77, 0x88};
  int fd;
  size_t data_start = 52 + 40 * 6;

  ehdr[0] = ELFMAG0;
  ehdr[1] = ELFMAG1;
  ehdr[2] = ELFMAG2;
  ehdr[3] = ELFMAG3;
  ehdr[4] = ELFCLASS32;
  ehdr[5] = ELFDATA2LSB;
  ehdr[6] = EV_CURRENT;
  ehdr[16] = ET_REL & 0xff;
  ehdr[17] = (ET_REL >> 8) & 0xff;
  ehdr[18] = EM_ARM & 0xff;
  ehdr[19] = (EM_ARM >> 8) & 0xff;
  ehdr[20] = EV_CURRENT & 0xff;
  ehdr[32] = 52 & 0xff;
  ehdr[33] = (52 >> 8) & 0xff;
  ehdr[34] = (52 >> 16) & 0xff;
  ehdr[35] = (52 >> 24) & 0xff;
  ehdr[40] = 52 & 0xff;
  ehdr[41] = (52 >> 8) & 0xff;
  ehdr[46] = 40 & 0xff;
  ehdr[47] = (40 >> 8) & 0xff;
  ehdr[48] = 6 & 0xff;
  ehdr[49] = (6 >> 8) & 0xff;
  ehdr[50] = 5 & 0xff;
  ehdr[51] = (5 >> 8) & 0xff;

  shdr[1 * 40 + 0] = 9;
  shdr[1 * 40 + 4] = SHT_PROGBITS;
  shdr[1 * 40 + 8] = SHF_ALLOC | SHF_EXECINSTR;
  ut_write32le(shdr + 1 * 40 + 16, data_start);
  shdr[1 * 40 + 20] = sizeof(text);
  shdr[1 * 40 + 32] = 1;

  shdr[2 * 40 + 0] = 17;
  shdr[2 * 40 + 4] = SHT_PROGBITS;
  shdr[2 * 40 + 8] = 0;
  ut_write32le(shdr + 2 * 40 + 16, data_start + sizeof(text));
  shdr[2 * 40 + 20] = sizeof(debug);
  shdr[2 * 40 + 32] = 1;

  shdr[3 * 40 + 0] = 1;
  shdr[3 * 40 + 4] = SHT_SYMTAB;
  ut_write32le(shdr + 3 * 40 + 16, data_start + sizeof(text) + sizeof(debug));
  shdr[3 * 40 + 20] = sizeof(symtab);
  shdr[3 * 40 + 24] = 4;
  shdr[3 * 40 + 36] = 16;

  shdr[4 * 40 + 0] = 29;
  shdr[4 * 40 + 4] = SHT_STRTAB;
  ut_write32le(shdr + 4 * 40 + 16, data_start + sizeof(text) + sizeof(debug) + sizeof(symtab));
  shdr[4 * 40 + 20] = sizeof(strtab);

  shdr[5 * 40 + 0] = 37;
  shdr[5 * 40 + 4] = SHT_STRTAB;
  ut_write32le(shdr + 5 * 40 + 16, data_start + sizeof(text) + sizeof(debug) + sizeof(symtab) + sizeof(strtab));
  shdr[5 * 40 + 20] = sizeof(shstrtab);

  memcpy(shstrtab + 1, ".symtab", 8);
  memcpy(shstrtab + 9, ".text", 6);
  memcpy(shstrtab + 17, ".debug_info", 12);
  memcpy(shstrtab + 29, ".strtab", 8);
  memcpy(shstrtab + 37, ".shstrtab", 10);

  memcpy(strtab + 1, sym_name, strlen(sym_name) + 1);

  symtab[16 + 0] = 1 & 0xff;
  symtab[16 + 1] = (1 >> 8) & 0xff;
  symtab[16 + 4] = 0;
  symtab[16 + 8] = sizeof(text) & 0xff;
  symtab[16 + 12] = ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC);
  symtab[16 + 14] = 1 & 0xff;
  symtab[16 + 15] = (1 >> 8) & 0xff;

  unsigned char file[512] = {0};
  size_t off = 0;
  memcpy(file + off, ehdr, sizeof(ehdr));
  off += sizeof(ehdr);
  memcpy(file + off, shdr, sizeof(shdr));
  off += sizeof(shdr);
  memcpy(file + off, text, sizeof(text));
  off += sizeof(text);
  memcpy(file + off, debug, sizeof(debug));
  off += sizeof(debug);
  memcpy(file + off, symtab, sizeof(symtab));
  off += sizeof(symtab);
  memcpy(file + off, strtab, sizeof(strtab));
  off += sizeof(strtab);
  memcpy(file + off, shstrtab, sizeof(shstrtab));
  off += sizeof(shstrtab);

  UT_ASSERT(ut_make_temp_file("tccelf_ut_elfdbg_", file, off, path_out, path_size) == 0);
  fd = open(path_out, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  return fd;
}

/* Build a tiny ARM ELF with a relocation section pointing at .text.  Used to
 * exercise the eager loader's third relocation-repair pass. */
static int ut_make_minimal_elf_o_with_rel(const char *sym_name, char *path_out, size_t path_size)
{
  unsigned char ehdr[52] = {0};
  unsigned char shdr[40 * 6] = {0};
  unsigned char shstrtab[64] = {0};
  unsigned char strtab[64] = {0};
  unsigned char symtab[2 * 16] = {0};
  unsigned char text[4] = {0x11, 0x22, 0x33, 0x44};
  unsigned char rel[8] = {0}; /* one Elf32_Rel */
  int fd;
  size_t data_start = 52 + 40 * 6;

  ehdr[0] = ELFMAG0;
  ehdr[1] = ELFMAG1;
  ehdr[2] = ELFMAG2;
  ehdr[3] = ELFMAG3;
  ehdr[4] = ELFCLASS32;
  ehdr[5] = ELFDATA2LSB;
  ehdr[6] = EV_CURRENT;
  ehdr[16] = ET_REL & 0xff;
  ehdr[17] = (ET_REL >> 8) & 0xff;
  ehdr[18] = EM_ARM & 0xff;
  ehdr[19] = (EM_ARM >> 8) & 0xff;
  ehdr[20] = EV_CURRENT & 0xff;
  ehdr[32] = 52 & 0xff;
  ehdr[33] = (52 >> 8) & 0xff;
  ehdr[34] = (52 >> 16) & 0xff;
  ehdr[35] = (52 >> 24) & 0xff;
  ehdr[40] = 52 & 0xff;
  ehdr[41] = (52 >> 8) & 0xff;
  ehdr[46] = 40 & 0xff;
  ehdr[47] = (40 >> 8) & 0xff;
  ehdr[48] = 6 & 0xff;
  ehdr[49] = (6 >> 8) & 0xff;
  ehdr[50] = 5 & 0xff;
  ehdr[51] = (5 >> 8) & 0xff;

  /* .text */
  shdr[1 * 40 + 0] = 9;
  shdr[1 * 40 + 4] = SHT_PROGBITS;
  shdr[1 * 40 + 8] = SHF_ALLOC | SHF_EXECINSTR;
  ut_write32le(shdr + 1 * 40 + 16, data_start);
  shdr[1 * 40 + 20] = sizeof(text);
  shdr[1 * 40 + 32] = 1;

  /* .rel.text */
  shdr[2 * 40 + 0] = 15;
  shdr[2 * 40 + 4] = SHT_RELX;
  shdr[2 * 40 + 8] = SHF_ALLOC;
  ut_write32le(shdr + 2 * 40 + 16, data_start + sizeof(text));
  shdr[2 * 40 + 20] = sizeof(rel);
  shdr[2 * 40 + 24] = 3; /* sh_link -> .symtab */
  shdr[2 * 40 + 28] = 1; /* sh_info -> .text */
  shdr[2 * 40 + 36] = 8; /* sh_entsize */

  /* .symtab */
  shdr[3 * 40 + 0] = 1;
  shdr[3 * 40 + 4] = SHT_SYMTAB;
  ut_write32le(shdr + 3 * 40 + 16, data_start + sizeof(text) + sizeof(rel));
  shdr[3 * 40 + 20] = sizeof(symtab);
  shdr[3 * 40 + 24] = 4; /* sh_link -> .strtab */
  shdr[3 * 40 + 36] = 16;

  /* .strtab */
  shdr[4 * 40 + 0] = 25;
  shdr[4 * 40 + 4] = SHT_STRTAB;
  ut_write32le(shdr + 4 * 40 + 16, data_start + sizeof(text) + sizeof(rel) + sizeof(symtab));
  shdr[4 * 40 + 20] = sizeof(strtab);

  /* .shstrtab */
  shdr[5 * 40 + 0] = 33;
  shdr[5 * 40 + 4] = SHT_STRTAB;
  ut_write32le(shdr + 5 * 40 + 16, data_start + sizeof(text) + sizeof(rel) + sizeof(symtab) + sizeof(strtab));
  shdr[5 * 40 + 20] = sizeof(shstrtab);

  memcpy(shstrtab + 1, ".symtab", 8);
  memcpy(shstrtab + 9, ".text", 6);
  memcpy(shstrtab + 15, ".rel.text", 10);
  memcpy(shstrtab + 25, ".strtab", 8);
  memcpy(shstrtab + 33, ".shstrtab", 10);

  memcpy(strtab + 1, sym_name, strlen(sym_name) + 1);

  /* Symbol 1: global function in .text */
  symtab[16 + 0] = 1 & 0xff;
  symtab[16 + 1] = (1 >> 8) & 0xff;
  symtab[16 + 4] = 0;
  symtab[16 + 8] = sizeof(text) & 0xff;
  symtab[16 + 12] = ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC);
  symtab[16 + 14] = 1 & 0xff;
  symtab[16 + 15] = (1 >> 8) & 0xff;

  /* Relocation against symbol 1 at offset 0 */
  ut_write32le(rel + 0, 0); /* r_offset */
  ut_write32le(rel + 4, ELFW(R_INFO)(1, R_DATA_PTR));

  unsigned char file[512] = {0};
  size_t off = 0;
  memcpy(file + off, ehdr, sizeof(ehdr));
  off += sizeof(ehdr);
  memcpy(file + off, shdr, sizeof(shdr));
  off += sizeof(shdr);
  memcpy(file + off, text, sizeof(text));
  off += sizeof(text);
  memcpy(file + off, rel, sizeof(rel));
  off += sizeof(rel);
  memcpy(file + off, symtab, sizeof(symtab));
  off += sizeof(symtab);
  memcpy(file + off, strtab, sizeof(strtab));
  off += sizeof(strtab);
  memcpy(file + off, shstrtab, sizeof(shstrtab));
  off += sizeof(shstrtab);

  UT_ASSERT(ut_make_temp_file("tccelf_ut_elfrel_", file, off, path_out, path_size) == 0);
  fd = open(path_out, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  return fd;
}

/* Patch a minimal ELF produced by ut_make_minimal_elf_o so that section 1
 * (normally .text) has a new name, type and flags.  This lets the same helper
 * drive tests for merged names, section type conflicts, .gnu.linkonce,
 * mandatory-section detection and stab deferral without duplicating the ELF
 * construction code. */
static int ut_patch_minimal_elf_section1(const char *path, const char *new_name, int new_type, int new_flags)
{
  int fd;
  unsigned char file[512];
  ssize_t n;
  size_t shstr_off, append_off;
  size_t name_len = strlen(new_name) + 1;

  fd = open(path, O_RDWR | O_BINARY);
  if (fd < 0)
    return -1;
  n = full_read(fd, file, sizeof(file));
  close(fd);
  if (n <= 52 + 40 * 4)
    return -1;

  /* .shstrtab section header is index 3; sh_offset is at byte 16 of the header. */
  shstr_off = file[52 + 3 * 40 + 16] |
              ((size_t)file[52 + 3 * 40 + 17] << 8) |
              ((size_t)file[52 + 3 * 40 + 18] << 16) |
              ((size_t)file[52 + 3 * 40 + 19] << 24);

  /* Original strtab layout: 0="", 1=".symtab", 9=".text", 17=".shstrtab",
   * 28=symbol name.  Append the new section name after the symbol name. */
  append_off = 28;
  while (append_off < 64 && file[shstr_off + append_off] != '\0')
    append_off++;
  append_off++; /* skip terminating NUL of symbol name */
  if (append_off + name_len > 64)
    return -1;
  memcpy(file + shstr_off + append_off, new_name, name_len);

  /* Section header 1 starts at 52 + 1*40. */
  file[52 + 1 * 40 + 0] = append_off & 0xff;
  file[52 + 1 * 40 + 1] = (append_off >> 8) & 0xff;
  file[52 + 1 * 40 + 2] = (append_off >> 16) & 0xff;
  file[52 + 1 * 40 + 3] = (append_off >> 24) & 0xff;

  file[52 + 1 * 40 + 4] = new_type & 0xff;
  file[52 + 1 * 40 + 5] = (new_type >> 8) & 0xff;
  file[52 + 1 * 40 + 6] = (new_type >> 16) & 0xff;
  file[52 + 1 * 40 + 7] = (new_type >> 24) & 0xff;

  file[52 + 1 * 40 + 8] = new_flags & 0xff;
  file[52 + 1 * 40 + 9] = (new_flags >> 8) & 0xff;
  file[52 + 1 * 40 + 10] = (new_flags >> 16) & 0xff;
  file[52 + 1 * 40 + 11] = (new_flags >> 24) & 0xff;

  fd = open(path, O_WRONLY | O_TRUNC | O_BINARY);
  if (fd < 0)
    return -1;
  if ((ssize_t)write(fd, file, n) != n)
  {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

UT_TEST(test_tcc_load_object_file_lazy_loads_symbols)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  char path[64];
  int fd = ut_make_minimal_elf_o("lazy_sym", path, sizeof(path));

  int rc = tcc_load_object_file_lazy(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, 0);
  UT_ASSERT_EQ(tcc_state->nb_lazy_objfiles, 1);

  int idx = find_elf_sym(symtab_section, "lazy_sym");
  UT_ASSERT(idx != 0);

  tcc_free_lazy_objfiles(tcc_state);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_gc_mark_and_load_referenced_sections)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  char path[64];
  int fd = ut_make_minimal_elf_o("main", path, sizeof(path));

  int rc = tcc_load_object_file_lazy(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, 0);

  tcc_state->gc_sections_aggressive = 1;
  tcc_gc_mark_phase(tcc_state);
  tcc_load_referenced_sections(tcc_state);

  LazyObjectFile *obj = tcc_state->lazy_objfiles[0];
  UT_ASSERT(obj->sections[1].referenced);
  UT_ASSERT(obj->sections[1].section != NULL);
  UT_ASSERT_EQ(obj->sections[1].section->data_offset, 4);

  tcc_free_lazy_objfiles(tcc_state);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_referenced_sections_leaves_unreferenced_debug_unloaded)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  char path[64];
  int fd = ut_make_minimal_elf_o_with_debug("main", path, sizeof(path));

  int rc = tcc_load_object_file_lazy(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, 0);

  tcc_state->gc_sections_aggressive = 1;
  tcc_gc_mark_phase(tcc_state);
  tcc_load_referenced_sections(tcc_state);

  LazyObjectFile *obj = tcc_state->lazy_objfiles[0];
  UT_ASSERT(obj->sections[1].referenced);
  UT_ASSERT(obj->sections[1].section != NULL);
  UT_ASSERT(!obj->sections[2].referenced);
  UT_ASSERT(obj->sections[2].section == NULL);

  tcc_free_lazy_objfiles(tcc_state);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_free_lazy_objfiles_resets_loaded_list)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  char path[64];
  int fd = ut_make_minimal_elf_o("lazy_free_sym", path, sizeof(path));

  int rc = tcc_load_object_file_lazy(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, 0);
  UT_ASSERT(tcc_state->lazy_objfiles != NULL);
  UT_ASSERT_EQ(tcc_state->nb_lazy_objfiles, 1);

  tcc_free_lazy_objfiles(tcc_state);

  UT_ASSERT(tcc_state->lazy_objfiles == NULL);
  UT_ASSERT_EQ(tcc_state->nb_lazy_objfiles, 0);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Relocate sections (drives relocate_section and add_reloc_patch)
 * ============================================================================ */

UT_TEST(test_relocate_sections_calls_relocate_per_entry)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  text_section->sh_addr = 0x1000;
  section_ptr_add(text_section, 4);

  int idx = set_elf_sym(symtab_section, 0x12345678, 4,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                        0, text_section->sh_num, "relocated_sym");

  put_elf_reloc(symtab_section, text_section, 0, R_DATA_PTR, idx);

  ut_reloc_call_count = 0;
  ut_reloc_last_tgt = 0;
  relocate_sections(tcc_state);

  UT_ASSERT_EQ(ut_reloc_call_count, 1);
  UT_ASSERT_EQ(ut_reloc_last_tgt, (addr_t)0x12345678);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_relocate_sections_adjusts_alloc_relocs)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  text_section->sh_addr = 0x1000;
  section_ptr_add(text_section, 4);

  int idx = set_elf_sym(symtab_section, 0x22222222, 4,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                        0, text_section->sh_num, "alloc_reloc_sym");

  put_elf_reloc(symtab_section, text_section, 0, R_DATA_PTR, idx);
  /* Mark the relocation section as allocated to exercise the r_offset
   * adjustment path in relocate_sections. */
  text_section->reloc->sh_flags |= SHF_ALLOC;

  ut_reloc_call_count = 0;
  relocate_sections(tcc_state);

  UT_ASSERT_EQ(ut_reloc_call_count, 1);
  ElfW_Rel *rel = (ElfW_Rel *)text_section->reloc->data;
  UT_ASSERT_EQ(rel->r_offset, (addr_t)0x1000); /* 0 + sh_addr */

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Eager object-file loading (drives should_defer_section, section_add_deferred,
 * find_existing_section and section_ht_find)
 * ============================================================================ */

UT_TEST(test_tcc_load_object_file_with_debug_section)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  char path[64];
  int fd = ut_make_minimal_elf_o_with_debug("debug_sym", path, sizeof(path));

  tcc_state->do_debug = 1;
  tcc_state->current_filename = tcc_strdup(path);

  int rc = tcc_load_object_file(tcc_state, fd, 0);
  close(fd);

  UT_ASSERT_EQ(rc, 0);

  /* Symbol was merged into the global symbol table. */
  int idx = find_elf_sym(symtab_section, "debug_sym");
  UT_ASSERT(idx != 0);

  /* .text data was merged immediately. */
  Section *txt = find_section(tcc_state, ".text");
  UT_ASSERT(txt->data_offset >= 4);
  UT_ASSERT_EQ(txt->data[0], 0x11);

  /* .debug_info was deferred (lazy chunks recorded). */
  Section *dbg = find_section(tcc_state, ".debug_info");
  UT_ASSERT(dbg != NULL);
  UT_ASSERT(dbg->lazy);
  UT_ASSERT(dbg->has_deferred_chunks);
  UT_ASSERT(dbg->deferred_head != NULL);
  UT_ASSERT_EQ(dbg->deferred_head->size, 4u);

  tcc_free((void *)tcc_state->current_filename);
  unlink(path);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_object_file_finds_existing_section)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  /* Pre-populate .text so the loader must use find_existing_section. */
  section_ptr_add(text_section, 4);
  text_section->data[0] = 0xaa;

  char path[64];
  int fd = ut_make_minimal_elf_o("merge_sym", path, sizeof(path));

  int rc = tcc_load_object_file(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, 0);

  /* The existing .text was extended, not replaced. */
  UT_ASSERT(text_section->data_offset >= 8);
  UT_ASSERT_EQ(text_section->data[0], 0xaa);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_object_file_with_relocation_section)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  char path[64];
  int fd = ut_make_minimal_elf_o_with_rel("reloc_sym", path, sizeof(path));

  int rc = tcc_load_object_file(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, 0);

  /* The symbol was added to the global symtab. */
  int new_idx = find_elf_sym(symtab_section, "reloc_sym");
  UT_ASSERT(new_idx != 0);

  /* A .rel.text section was created and attached to text_section. */
  UT_ASSERT(text_section->reloc != NULL);
  UT_ASSERT_STREQ(text_section->reloc->name, ".rel.text");
  UT_ASSERT_EQ(text_section->reloc->sh_type, SHT_RELX);

  /* The relocation's symbol index was rewritten from the old local symtab
   * index (1) to the new global symtab index. */
  ElfW_Rel *rel = (ElfW_Rel *)text_section->reloc->data;
  UT_ASSERT_EQ(ELFW(R_SYM)(rel->r_info), new_idx);
  UT_ASSERT_EQ(ELFW(R_TYPE)(rel->r_info), R_DATA_PTR);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_object_file_merges_named_subsections)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  struct {
    const char *name;
    int type;
    int flags;
    const char *sym_name;
  } cases[] = {
    { ".text.foo", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, "text_sym" },
    { ".data.foo", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,     "data_sym" },
    { ".rodata.foo", SHT_PROGBITS, SHF_ALLOC,               "rodata_sym" },
    { ".bss.foo", SHT_NOBITS, SHF_ALLOC | SHF_WRITE,        "bss_sym" },
  };

  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
  {
    char path[64];
    int fd = ut_make_minimal_elf_o(cases[c].sym_name, path, sizeof(path));
    close(fd);
    UT_ASSERT_EQ(ut_patch_minimal_elf_section1(path, cases[c].name, cases[c].type, cases[c].flags), 0);

    fd = open(path, O_RDONLY | O_BINARY);
    UT_ASSERT(fd >= 0);
    int rc = tcc_load_object_file(tcc_state, fd, 0);
    close(fd);
    unlink(path);
    UT_ASSERT_EQ(rc, 0);
  }

  /* Each merged subsection extended its canonical parent. */
  UT_ASSERT(text_section->data_offset >= 4);
  UT_ASSERT(data_section->data_offset >= 4);
  UT_ASSERT(rodata_section->data_offset >= 4);
  UT_ASSERT(bss_section->data_offset >= 4);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_object_file_gnu_linkonce_skips_duplicate)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  /* Pre-create .text so the .gnu.linkonce section is treated as a duplicate. */
  section_ptr_add(text_section, 4);

  char path[64];
  int fd = ut_make_minimal_elf_o("linkonce_sym", path, sizeof(path));
  close(fd);
  UT_ASSERT_EQ(ut_patch_minimal_elf_section1(path, ".gnu.linkonce.t.foo", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR), 0);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  int rc = tcc_load_object_file(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, 0);
  /* .text size must not have grown: the linkonce section was skipped. */
  UT_ASSERT_EQ(text_section->data_offset, 4u);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_object_file_section_type_conflict)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  /* Pre-create .custom as PROGBITS, then load an object whose .custom is NOBITS. */
  Section *custom = new_section(tcc_state, ".custom", SHT_PROGBITS, SHF_ALLOC);
  section_ptr_add(custom, 4);

  char path[64];
  int fd = ut_make_minimal_elf_o("conflict_sym", path, sizeof(path));
  close(fd);
  UT_ASSERT_EQ(ut_patch_minimal_elf_section1(path, ".custom", SHT_NOBITS, SHF_ALLOC), 0);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  int rc = tcc_load_object_file(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, -1);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_object_file_invalid_type)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  unsigned char garbage[] = "not an elf";
  char path[64];
  UT_ASSERT(ut_make_temp_file("tccelf_ut_eager_badtype_", garbage, sizeof(garbage), path, sizeof(path)) == 0);

  int fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  int rc = tcc_load_object_file(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, -1);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_object_file_bad_machine)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  unsigned char ehdr[52] = {0};
  ehdr[0] = ELFMAG0;
  ehdr[1] = ELFMAG1;
  ehdr[2] = ELFMAG2;
  ehdr[3] = ELFMAG3;
  ehdr[4] = ELFCLASS32;
  ehdr[5] = ELFDATA2LSB;
  ehdr[6] = EV_CURRENT;
  ehdr[16] = ET_REL & 0xff;
  ehdr[17] = (ET_REL >> 8) & 0xff;
  ehdr[18] = EM_386 & 0xff;  /* wrong machine */
  ehdr[19] = (EM_386 >> 8) & 0xff;

  char path[64];
  UT_ASSERT(ut_make_temp_file("tccelf_ut_eager_badmach_", ehdr, sizeof(ehdr), path, sizeof(path)) == 0);

  int fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  int rc = tcc_load_object_file(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, -1);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_object_file_defers_stab_section)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  char path[64];
  int fd = ut_make_minimal_elf_o("stab_sym", path, sizeof(path));
  close(fd);
  UT_ASSERT_EQ(ut_patch_minimal_elf_section1(path, ".stab", SHT_PROGBITS, 0), 0);

  tcc_state->do_debug = 1;
  tcc_state->current_filename = tcc_strdup(path);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  int rc = tcc_load_object_file(tcc_state, fd, 0);
  close(fd);

  UT_ASSERT_EQ(rc, 0);

  Section *stab = find_section(tcc_state, ".stab");
  UT_ASSERT(stab != NULL);
  UT_ASSERT(stab->lazy);
  UT_ASSERT(stab->has_deferred_chunks);

  tcc_free((void *)tcc_state->current_filename);
  unlink(path);
  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * ELF object type detection (error branches)
 * ============================================================================ */

UT_TEST(test_tcc_object_type_short_header_returns_zero)
{
  char path[64];
  int fd, type;
  ElfW(Ehdr) h;

  UT_ASSERT(ut_make_temp_file("tccelf_ut_short_", (unsigned char *)"", 0, path, sizeof(path)) == 0);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  type = tcc_object_type(fd, &h);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(type, 0);
  return 0;
}

/* ============================================================================
 * Lazy object-file loading (error branches)
 * ============================================================================ */

UT_TEST(test_tcc_load_object_file_lazy_invalid_type)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  unsigned char garbage[] = "not an elf";
  char path[64];
  UT_ASSERT(ut_make_temp_file("tccelf_ut_badtype_", garbage, sizeof(garbage), path, sizeof(path)) == 0);

  int fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  int rc = tcc_load_object_file_lazy(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, -1);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_object_file_lazy_bad_machine)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  unsigned char ehdr[52] = {0};
  ehdr[0] = ELFMAG0;
  ehdr[1] = ELFMAG1;
  ehdr[2] = ELFMAG2;
  ehdr[3] = ELFMAG3;
  ehdr[4] = ELFCLASS32;
  ehdr[5] = ELFDATA2LSB;
  ehdr[6] = EV_CURRENT;
  ehdr[16] = ET_REL & 0xff;
  ehdr[17] = (ET_REL >> 8) & 0xff;
  ehdr[18] = EM_386 & 0xff;  /* wrong machine */
  ehdr[19] = (EM_386 >> 8) & 0xff;

  char path[64];
  UT_ASSERT(ut_make_temp_file("tccelf_ut_badmach_", ehdr, sizeof(ehdr), path, sizeof(path)) == 0);

  int fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  int rc = tcc_load_object_file_lazy(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, -1);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_load_object_file_lazy_multiple_symtabs)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  /* Reuse the debug ELF but add a second SHT_SYMTAB section. */
  char path[64];
  int fd_src = ut_make_minimal_elf_o_with_debug("dup_sym", path, sizeof(path));
  close(fd_src);

  /* Read the file, patch section header 2 (the real symtab) and append a
   * second symtab by reusing the same bytes. */
  int fd_r = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd_r >= 0);
  unsigned char file[512];
  ssize_t n = full_read(fd_r, file, sizeof(file));
  close(fd_r);
  UT_ASSERT(n > 52 + 40 * 6);

  /* Section count is at offset 48 (little-endian).  Bump from 6 to 7. */
  file[48] = 7;
  file[49] = 0;

  /* Copy the .symtab section header (index 3) to new index 6. */
  memcpy(file + 52 + 40 * 6, file + 52 + 40 * 3, 40);
  /* Name offset for the duplicate points to a name we'll append at the end
   * of .shstrtab.  Use offset 47 (after ".shstrtab\0") for "_dup". */
  file[52 + 40 * 6 + 0] = 47 & 0xff;
  file[52 + 40 * 6 + 1] = 0;
  file[52 + 40 * 6 + 2] = 0;
  file[52 + 40 * 6 + 3] = 0;

  /* Append "_dup\0" to .shstrtab.  Its offset in the file is stored in the
   * .shstrtab section header at 52 + 40*5 + 16 (sh_offset). */
  size_t shstr_off = file[52 + 40 * 5 + 16] |
                     ((size_t)file[52 + 40 * 5 + 17] << 8) |
                     ((size_t)file[52 + 40 * 5 + 18] << 16) |
                     ((size_t)file[52 + 40 * 5 + 19] << 24);
  memcpy(file + shstr_off + 47, "_dup", 5);

  int fd = open(path, O_WRONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  UT_ASSERT((ssize_t)write(fd, file, n) == n);
  close(fd);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  int rc = tcc_load_object_file_lazy(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, -1);
  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_tcc_free_lazy_objfiles_no_objfiles)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  /* The early-return branch when lazy_objfiles is NULL must be safe. */
  tcc_free_lazy_objfiles(tcc_state);
  UT_ASSERT_EQ(tcc_state->nb_lazy_objfiles, 0);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_section_is_mandatory_marks_init_fini_arrays)
{
  const char *names[] = {
    ".data", ".rodata", ".bss", ".init", ".fini",
    ".init_array", ".fini_array", ".preinit_array"
  };

  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
  {
    ut_elf_reset_state();
    tccelf_new(tcc_state);

    char path[64];
    int fd = ut_make_minimal_elf_o("mandatory_sym", path, sizeof(path));
    close(fd);

    /* Patch section 1 to one of the mandatory names so section_is_mandatory
     * is exercised for that branch. */
    UT_ASSERT_EQ(ut_patch_minimal_elf_section1(path, names[i], SHT_PROGBITS, SHF_ALLOC), 0);

    fd = open(path, O_RDONLY | O_BINARY);
    UT_ASSERT(fd >= 0);
    int rc = tcc_load_object_file_lazy(tcc_state, fd, 0);
    close(fd);
    unlink(path);

    UT_ASSERT_EQ(rc, 0);
    UT_ASSERT(tcc_state->nb_lazy_objfiles == 1);
    UT_ASSERT(tcc_state->lazy_objfiles[0]->sections[1].referenced);

    tcc_free_lazy_objfiles(tcc_state);
    tccelf_delete(tcc_state);
  }

  return 0;
}

/* ============================================================================
 * GOT filling (remaining branches)
 * ============================================================================ */

UT_TEST(test_fill_got_entry_zero_offset_skips)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  Section *got = new_section(tcc_state, ".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
  tcc_state->got = got;
  section_ptr_add(got, 8);
  memset(got->data, 0xcc, 8);

  int idx = set_elf_sym(symtab_section, 0xdeadbeef, 4,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                        0, text_section->sh_num, "got_skip");

  struct sym_attr *attr = get_sym_attr(tcc_state, idx, 1);
  attr->got_offset = 0; /* no GOT slot allocated */

  ElfW_Rel rel;
  rel.r_offset = 0;
  rel.r_info = ELFW(R_INFO)(idx, R_DATA_PTR);

  fill_got_entry(tcc_state, &rel);

  /* The 0 offset short-circuit must leave the GOT untouched. */
  for (int i = 0; i < 8; i++)
    UT_ASSERT_EQ(got->data[i], 0xcc);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * DWARF relocation handling
 * ============================================================================ */

UT_TEST(test_relocate_section_dwarf_lazy_adds_patches)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  Section *debug_str = new_section(tcc_state, ".debug_str", SHT_PROGBITS, 0);
  Section *debug_info = new_section(tcc_state, ".debug_info", SHT_PROGBITS, 0);

  /* Mark both as DWARF sections. */
  tcc_state->dwlo = debug_str->sh_num;
  tcc_state->dwhi = debug_info->sh_num + 1;

  debug_str->sh_addr = 0x100;
  section_ptr_add(debug_str, 16);

  int sym = set_elf_sym(symtab_section, 0x300, 4,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                        0, debug_str->sh_num, "dwarf_str_sym");

  /* Pre-allocate .debug_info data, then mark it lazy so patches are recorded
   * instead of being applied immediately. */
  section_ptr_add(debug_info, 4);
  debug_info->lazy = 1;
  debug_info->materialized = 0;

  put_elf_reloc(symtab_section, debug_info, 0, R_DATA_32DW, sym);

  relocate_sections(tcc_state);

  /* The lazy DWARF path must have recorded one patch with the dwarf-to-dwarf
   * value (sym_value - debug_str->sh_addr). */
  UT_ASSERT_EQ(debug_info->nb_reloc_patches, 1);
  UT_ASSERT_EQ(debug_info->reloc_patch_offsets[0], 0u);
  UT_ASSERT_EQ(debug_info->reloc_patch_values[0], 0x200u);

  /* Materializing should apply the patch to the zeroed buffer. */
  section_materialize(tcc_state, debug_info);
  UT_ASSERT_EQ(read32le(debug_info->data), 0x200u);

  tccelf_delete(tcc_state);
  return 0;
}

UT_TEST(test_relocate_section_dwarf_materialized_add32le)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  Section *debug_str = new_section(tcc_state, ".debug_str", SHT_PROGBITS, 0);
  Section *debug_info = new_section(tcc_state, ".debug_info", SHT_PROGBITS, 0);

  tcc_state->dwlo = debug_str->sh_num;
  tcc_state->dwhi = debug_info->sh_num + 1;

  debug_str->sh_addr = 0x1000;
  section_ptr_add(debug_str, 16);

  int sym = set_elf_sym(symtab_section, 0x1300, 4,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                        0, debug_str->sh_num, "dwarf_str_sym2");

  /* Allocate .debug_info data and pre-fill it so we can see add32le in action. */
  unsigned char *p = section_ptr_add(debug_info, 4);
  write32le(p, 0x10);

  put_elf_reloc(symtab_section, debug_info, 0, R_DATA_32DW, sym);

  relocate_sections(tcc_state);

  /* Materialized DWARF path updates data in place. */
  UT_ASSERT_EQ(debug_info->nb_reloc_patches, 0);
  UT_ASSERT_EQ(read32le(debug_info->data), 0x310u); /* 0x10 + (0x1300 - 0x1000) */

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * ARM-specific relocation skips
 * ============================================================================ */

UT_TEST(test_relocate_sections_skips_arm_exidx_without_alloc)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  Section *exidx = new_section(tcc_state, ".ARM.exidx", SHT_ARM_EXIDX, 0);
  section_ptr_add(exidx, 8);

  int sym = set_elf_sym(symtab_section, 0x1234, 4,
                        ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT),
                        0, text_section->sh_num, "exidx_target");

  put_elf_reloc(symtab_section, exidx, 0, R_DATA_PTR, sym);

  ut_reloc_call_count = 0;
  relocate_sections(tcc_state);

  UT_ASSERT_EQ(ut_reloc_call_count, 0);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Section creation edge cases
 * ============================================================================ */

UT_TEST(test_new_section_nobits_private)
{
  ut_elf_reset_state();
  ut_elf_init_minimal();

  Section *sec = new_section(tcc_state, ".mynobits", SHT_NOBITS, SHF_PRIVATE);
  UT_ASSERT(sec != NULL);
  UT_ASSERT_EQ(sec->sh_num, 0);
  UT_ASSERT(sec->data == NULL);
  UT_ASSERT_EQ(tcc_state->nb_priv_sections, 1);
  UT_ASSERT(tcc_state->priv_sections[0] == sec);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Symbol table edge cases
 * ============================================================================ */

UT_TEST(test_set_elf_sym_local_duplicate_allowed)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  int idx1 = set_elf_sym(symtab_section, 0x100, 4,
                         ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT),
                         STV_DEFAULT, text_section->sh_num, "local_dup");
  int idx2 = set_elf_sym(symtab_section, 0x200, 4,
                         ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT),
                         STV_DEFAULT, text_section->sh_num, "local_dup");

  UT_ASSERT_NE(idx1, idx2);
  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[idx1].st_value, 0x100);
  UT_ASSERT_EQ(syms[idx2].st_value, 0x200);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Per-file symbol/reloc lifecycle (remaining branches)
 * ============================================================================ */

UT_TEST(test_tccelf_end_file_restores_hash_empty)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  tccelf_begin_file(tcc_state);
  UT_ASSERT(symtab_section->hash == NULL);
  UT_ASSERT(symtab_section->reloc != NULL);

  tccelf_end_file(tcc_state);
  UT_ASSERT(symtab_section->hash != NULL);
  UT_ASSERT(symtab_section->reloc == NULL);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Linker symbols (remaining branches)
 * ============================================================================ */

UT_TEST(test_tcc_add_linker_symbols_includes_alloc_strtab)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  Section *strsec = new_section(tcc_state, ".mystrtab", SHT_STRTAB, SHF_ALLOC);
  section_ptr_add(strsec, 8);

  resolve_common_syms(tcc_state);

  int start = find_elf_sym(symtab_section, "__start_mystrtab");
  int stop = find_elf_sym(symtab_section, "__stop_mystrtab");
  UT_ASSERT(start != 0);
  UT_ASSERT(stop != 0);
  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
  UT_ASSERT_EQ(syms[start].st_value, 0);
  UT_ASSERT_EQ(syms[stop].st_value, 8);

  tccelf_delete(tcc_state);
  return 0;
}

/* ============================================================================
 * Eager object-file loading (NOBITS sections)
 * ============================================================================ */

UT_TEST(test_tcc_load_object_file_nobits_section)
{
  ut_elf_reset_state();
  tccelf_new(tcc_state);

  char path[64];
  int fd = ut_make_minimal_elf_o("nobits_sym", path, sizeof(path));
  close(fd);

  UT_ASSERT_EQ(ut_patch_minimal_elf_section1(path, ".mynobits", SHT_NOBITS, SHF_ALLOC), 0);

  fd = open(path, O_RDONLY | O_BINARY);
  UT_ASSERT(fd >= 0);
  int rc = tcc_load_object_file(tcc_state, fd, 0);
  close(fd);
  unlink(path);

  UT_ASSERT_EQ(rc, 0);

  Section *s = find_section(tcc_state, ".mynobits");
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->sh_type, SHT_NOBITS);
  UT_ASSERT_EQ(s->data_offset, 4u);
  UT_ASSERT(s->data == NULL);

  tccelf_delete(tcc_state);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(tccelf)
{
  /* Lazy section materialization */
  UT_RUN(test_section_materialize_loads_deferred_chunk);
  UT_RUN(test_section_materialize_honors_deferred_dest_offset);
  UT_RUN(test_section_materialize_nobits_no_chunks_marks_done);
  UT_RUN(test_section_ensure_loaded_frees_discarded_chunks);
  UT_RUN(test_apply_reloc_patches_during_materialize);

  /* Symbol relocation */
  UT_RUN(test_relocate_syms_adds_section_base);
  UT_RUN(test_relocate_syms_undefined_weak_zeroes);
  UT_RUN(test_resolve_common_syms_allocates_in_bss);
  UT_RUN(test_relocate_section_dwarf_lazy_adds_patches);
  UT_RUN(test_relocate_section_dwarf_materialized_add32le);
  UT_RUN(test_relocate_sections_skips_arm_exidx_without_alloc);

  /* GOT filling */
  UT_RUN(test_fill_got_entry_writes_symbol_value);
  UT_RUN(test_fill_got_entry_zero_offset_skips);

  /* ELF object type detection */
  UT_RUN(test_tcc_object_type_detects_rel_elf);
  UT_RUN(test_tcc_object_type_unrecognized_returns_zero);
  UT_RUN(test_tcc_object_type_wrong_class_rejected);
  UT_RUN(test_full_read_loads_exact_bytes);
  UT_RUN(test_load_data_reads_from_offset);

  /* Standard linker symbols */
  UT_RUN(test_ld_export_standard_symbols_exports_section_boundaries);

  /* Section creation and lookup */
  UT_RUN(test_new_section_creates_named_typed_section);
  UT_RUN(test_new_section_private_goes_to_priv_sections);
  UT_RUN(test_find_section_creates_missing_section);
  UT_RUN(test_section_hash_table_grows_and_still_finds_sections);
  UT_RUN(test_new_section_nobits_private);

  /* Section data allocation */
  UT_RUN(test_section_add_allocates_and_aligns);
  UT_RUN(test_section_add_nobits_does_not_allocate_data);
  UT_RUN(test_section_ptr_add_returns_writable_pointer);
  UT_RUN(test_section_realloc_rounds_up_to_power_of_two);
  UT_RUN(test_section_prealloc_reserves_capacity_without_moving_offset);
  UT_RUN(test_section_add_updates_sh_addralign);

  /* String tables */
  UT_RUN(test_put_elf_str_appends_and_returns_offsets);

  /* Symbol tables and ELF hashing */
  UT_RUN(test_new_symtab_initializes_hash_and_first_symbol);
  UT_RUN(test_put_elf_sym_adds_local_and_global_symbols);
  UT_RUN(test_find_elf_sym_locates_added_symbols);
  UT_RUN(test_set_elf_sym_adds_new_local_symbol);
  UT_RUN(test_set_elf_sym_patches_existing_undefined_to_defined);
  UT_RUN(test_set_elf_sym_detects_duplicate_global_definition);
  UT_RUN(test_set_elf_sym_local_duplicate_allowed);

  /* Symbol attributes */
  UT_RUN(test_get_sym_attr_grows_array_and_zeroes_new_entries);

  /* Symbol table sorting */
  UT_RUN(test_tcc_elf_sort_syms_moves_locals_first);

  /* Relocations */
  UT_RUN(test_put_elf_reloc_creates_relocation_section);
  UT_RUN(test_put_elf_reloca_rejects_nonzero_addend_on_rel_arch);
  UT_RUN(test_put_elf_reloca_skips_invalid_symbol_index);

  /* Per-file symbol/reloc lifecycle */
  UT_RUN(test_tccelf_begin_file_saves_offsets_and_disables_hash);
  UT_RUN(test_tccelf_end_file_converts_local_undef_to_global);
  UT_RUN(test_tccelf_end_file_updates_relocations_after_symbol_rebuild);
  UT_RUN(test_tccelf_end_file_sets_undef_func_to_notype_for_obj_output);
  UT_RUN(test_tccelf_end_file_restores_hash_empty);

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
  UT_RUN(test_tccelf_delete_resets_sym_attrs);

  /* tccelf_new optional branches */
  UT_RUN(test_tccelf_new_creates_bounds_sections_when_enabled);
  UT_RUN(test_tccelf_new_calls_debug_new_when_enabled);

  /* Section type-specific alignment */
  UT_RUN(test_new_section_sets_sh_addralign_by_type);

  /* String tables */
  UT_RUN(test_put_elf_str_appends_duplicates);

  /* Symbol tables and ELF hashing */
  UT_RUN(test_put_elf_sym_rejects_invalid_first_byte);
  UT_RUN(test_put_elf_sym_hash_table_rebuilds_after_many_globals);
  UT_RUN(test_put_elf_sym_tracks_undefined_globals);

  /* set_elf_sym duplicate-definition policy branches */
  UT_RUN(test_set_elf_sym_identical_redefinition_returns_same_index);
  UT_RUN(test_set_elf_sym_global_overrides_weak);
  UT_RUN(test_set_elf_sym_weak_ignored_when_global_exists);
  UT_RUN(test_set_elf_sym_first_weak_kept);
  UT_RUN(test_set_elf_sym_hidden_ignored_after_defined);
  UT_RUN(test_set_elf_sym_data_takes_precedence_over_bss);
  UT_RUN(test_set_elf_sym_data_keeps_precedence_over_common);
  UT_RUN(test_set_elf_sym_common_to_data_takes_precedence);
  UT_RUN(test_set_elf_sym_visibility_propagation_weak_to_global);
  UT_RUN(test_set_elf_sym_visibility_default_after_nondefault);
  UT_RUN(test_set_elf_sym_visibility_both_nondefault);
  UT_RUN(test_set_elf_sym_asm_set_overridden);

  /* Symbol resolution helpers */
  UT_RUN(test_get_sym_addr_err_reports_undefined);
  UT_RUN(test_get_sym_addr_with_leading_underscore);
  UT_RUN(test_tcc_list_symbols_wrapper_lists_symbols);
  UT_RUN(test_set_global_sym_null_name_creates_local_absolute);

  /* Bound checking helper */
  UT_RUN(test_tcc_add_bcheck_noop_when_bounds_disabled);
  UT_RUN(test_tcc_add_bcheck_adds_when_bounds_enabled);

  /* File helpers */
  UT_RUN(test_full_read_returns_error_on_bad_fd);

  /* ELF object type detection (remaining branches) */
  UT_RUN(test_tcc_object_type_detects_dyn_elf);
  UT_RUN(test_tcc_object_type_detects_archive);
  UT_RUN(test_tcc_object_type_detects_yaff);
  UT_RUN(test_tcc_object_type_short_header_returns_zero);
  UT_RUN(test_tcc_object_type_exec_elf_returns_zero);

  /* Symbol relocation (remaining branches) */
  UT_RUN(test_relocate_syms_skips_undef_when_resolving_dynsym);
  UT_RUN(test_relocate_syms_rejects_invalid_st_name);
  UT_RUN(test_relocate_syms_accepts_fp_hw_undefined);
  UT_RUN(test_relocate_syms_reports_undefined_non_weak);
  UT_RUN(test_relocate_sections_calls_relocate_per_entry);
  UT_RUN(test_relocate_sections_adjusts_alloc_relocs);

  /* Lazy section materialization (remaining branches) */
  UT_RUN(test_section_ensure_loaded_materializes_lazy_section);
  UT_RUN(test_section_materialize_skips_already_materialized);
  UT_RUN(test_section_materialize_skips_materialized_chunk);
  UT_RUN(test_section_materialize_handles_open_failure);
  UT_RUN(test_section_materialize_handles_short_read);

  /* Section lifecycle */
  UT_RUN(test_free_section_null_is_safe);

  /* Linker symbols and common-symbol resolution */
  UT_RUN(test_ld_export_standard_symbols_updates_existing);
  UT_RUN(test_resolve_common_syms_with_init_array_section);
  UT_RUN(test_tcc_add_linker_symbols_skips_non_c_id_sections);
  UT_RUN(test_tcc_add_linker_symbols_includes_alloc_strtab);
  UT_RUN(test_rebuild_hash_handles_local_symbols);

  /* Lazy object-file loading and GC */
  UT_RUN(test_tcc_load_object_file_lazy_loads_symbols);
  UT_RUN(test_tcc_gc_mark_and_load_referenced_sections);
  UT_RUN(test_tcc_load_referenced_sections_leaves_unreferenced_debug_unloaded);
  UT_RUN(test_tcc_free_lazy_objfiles_resets_loaded_list);
  UT_RUN(test_tcc_load_object_file_lazy_invalid_type);
  UT_RUN(test_tcc_load_object_file_lazy_bad_machine);
  UT_RUN(test_tcc_load_object_file_lazy_multiple_symtabs);
  UT_RUN(test_tcc_free_lazy_objfiles_no_objfiles);
  UT_RUN(test_section_is_mandatory_marks_init_fini_arrays);

  /* Eager object-file loading */
  UT_RUN(test_tcc_load_object_file_with_debug_section);
  UT_RUN(test_tcc_load_object_file_finds_existing_section);
  UT_RUN(test_tcc_load_object_file_with_relocation_section);
  UT_RUN(test_tcc_load_object_file_merges_named_subsections);
  UT_RUN(test_tcc_load_object_file_gnu_linkonce_skips_duplicate);
  UT_RUN(test_tcc_load_object_file_section_type_conflict);
  UT_RUN(test_tcc_load_object_file_invalid_type);
  UT_RUN(test_tcc_load_object_file_bad_machine);
  UT_RUN(test_tcc_load_object_file_defers_stab_section);
  UT_RUN(test_tcc_load_object_file_nobits_section);
}
