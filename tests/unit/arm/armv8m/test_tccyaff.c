/*
 *  test_tccyaff.c - white-box unit tests for tccyaff.c
 *  (build_tccyaff/run_unit_tests_tccyaff)
 *
 *  Tests the pure helpers, the YAFF hash-table data structure, and selected
 *  higher-level load/resolve/free paths against a hand-built YAFF file.
 */

#define _DEFAULT_SOURCE
#define USING_GLOBALS

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tcc.h"
#include "tccyaff.h"

#include "ut.h"

/* tccyaff.h defines the data structures but not the helper prototypes; declare
 * the test-visible surface here. */
uint32_t tcc_yaff_hash(const char *name);
int tcc_output_yaff(TCCState *s1, FILE *f, const char *filename);
void tcc_yaff_prepare_init_fini(TCCState *s1);

/* Private section flags mirrored from tccelf.c / tccyaff.c. */
#ifndef SHF_PRIVATE
#define SHF_PRIVATE 0x80000000
#endif
#ifndef SHF_DYNSYM
#define SHF_DYNSYM 0x40000000
#endif
void tcc_allocate_hash_table(YaffHashTable *ht, uint32_t number_of_buckets, uint32_t count);
void tcc_add_hash_entry(YaffHashTable *ht, const char *name, uint32_t i);
void tcc_free_hash_table(YaffHashTable *ht);
void tcc_write_hash_table(YaffHashTable *ht, FILE *f);
uint32_t tcc_yaff_align(YaffHeader *header, uint32_t size);
const char *tcc_parse_object_name(YaffHeader *header);
uint32_t tcc_get_offset_to_imported_libraries(YaffHeader *header);

/* ============================================================================
 * Pure helpers
 * ============================================================================ */

UT_TEST(test_yaff_hash_empty)
{
  UT_ASSERT_EQ(tcc_yaff_hash(""), 0u);
  return 0;
}

UT_TEST(test_yaff_hash_simple_strings)
{
  /* Hand-traced ELF hash for short lowercase strings that do not overflow
   * the 0xf0000000 guard. */
  UT_ASSERT_EQ(tcc_yaff_hash("a"), 0x61u);
  UT_ASSERT_EQ(tcc_yaff_hash("ab"), (uint32_t)((0x61u << 4) + 0x62u));
  UT_ASSERT_EQ(tcc_yaff_hash("main"), 0x737feu);
  return 0;
}

UT_TEST(test_yaff_align_power_of_two)
{
  YaffHeader h = { .alignment = 4 };
  UT_ASSERT_EQ(tcc_yaff_align(&h, 0), 0u);
  UT_ASSERT_EQ(tcc_yaff_align(&h, 1), 4u);
  UT_ASSERT_EQ(tcc_yaff_align(&h, 3), 4u);
  UT_ASSERT_EQ(tcc_yaff_align(&h, 4), 4u);
  UT_ASSERT_EQ(tcc_yaff_align(&h, 5), 8u);

  h.alignment = 8;
  UT_ASSERT_EQ(tcc_yaff_align(&h, 0), 0u);
  UT_ASSERT_EQ(tcc_yaff_align(&h, 1), 8u);
  UT_ASSERT_EQ(tcc_yaff_align(&h, 8), 8u);
  UT_ASSERT_EQ(tcc_yaff_align(&h, 9), 16u);
  return 0;
}

UT_TEST(test_parse_object_name)
{
  /* A fake header followed by the object name. */
  char buf[256];
  memset(buf, 0, sizeof(buf));
  YaffHeader *h = (YaffHeader *)buf;
  strcpy(buf + sizeof(YaffHeader), "libfoo.yaff");
  UT_ASSERT_STREQ(tcc_parse_object_name(h), "libfoo.yaff");
  return 0;
}

UT_TEST(test_get_offset_to_imported_libraries)
{
  char buf[256];
  memset(buf, 0, sizeof(buf));
  YaffHeader *h = (YaffHeader *)buf;
  h->alignment = 4;
  strcpy(buf + sizeof(YaffHeader), "bar"); /* len 4 incl null -> aligned 4 */
  uint32_t expected = (uint32_t)sizeof(YaffHeader) + 4;
  UT_ASSERT_EQ(tcc_get_offset_to_imported_libraries(h), expected);

  strcpy(buf + sizeof(YaffHeader), "b"); /* len 2 incl null -> aligned 4 */
  expected = (uint32_t)sizeof(YaffHeader) + 4;
  UT_ASSERT_EQ(tcc_get_offset_to_imported_libraries(h), expected);
  return 0;
}

/* ============================================================================
 * Hash table data structure
 * ============================================================================ */

UT_TEST(test_hash_table_allocate_zeroes)
{
  YaffHashTable ht;
  tcc_allocate_hash_table(&ht, 7, 10);
  UT_ASSERT_EQ(ht.nbucket, 7u);
  UT_ASSERT_EQ(ht.nchain, 10u);
  UT_ASSERT(ht.bucket != NULL);
  UT_ASSERT(ht.chain != NULL);
  for (uint32_t i = 0; i < ht.nbucket; i++)
    UT_ASSERT_EQ(ht.bucket[i], 0u);
  for (uint32_t i = 0; i < ht.nchain; i++)
    UT_ASSERT_EQ(ht.chain[i], 0u);
  tcc_free_hash_table(&ht);
  return 0;
}

UT_TEST(test_hash_table_add_single)
{
  YaffHashTable ht;
  tcc_allocate_hash_table(&ht, 8, 8);
  tcc_add_hash_entry(&ht, "alpha", 3);
  uint32_t b = tcc_yaff_hash("alpha") % 8;
  UT_ASSERT_EQ(ht.bucket[b], 3u);
  tcc_free_hash_table(&ht);
  return 0;
}

UT_TEST(test_hash_table_add_collision_chains)
{
  /* Force a collision by using bucket count 1: every name lands in bucket 0. */
  YaffHashTable ht;
  tcc_allocate_hash_table(&ht, 1, 8);
  tcc_add_hash_entry(&ht, "first", 1);
  tcc_add_hash_entry(&ht, "second", 2);
  tcc_add_hash_entry(&ht, "third", 3);

  UT_ASSERT_EQ(ht.bucket[0], 1u);
  UT_ASSERT_EQ(ht.chain[1], 2u);
  UT_ASSERT_EQ(ht.chain[2], 3u);
  UT_ASSERT_EQ(ht.chain[3], 0u);

  tcc_free_hash_table(&ht);
  return 0;
}

UT_TEST(test_hash_table_write_and_readback)
{
  YaffHashTable ht;
  tcc_allocate_hash_table(&ht, 4, 4);
  tcc_add_hash_entry(&ht, "x", 1);

  char path[] = "/tmp/tccyaff_ut_hash_XXXXXX";
  int fd = mkstemp(path);
  UT_ASSERT(fd >= 0);
  FILE *f = fdopen(fd, "w+b");
  UT_ASSERT(f != NULL);

  tcc_write_hash_table(&ht, f);
  fflush(f);
  fseek(f, 0, SEEK_SET);

  uint32_t nbucket, nchain;
  UT_ASSERT_EQ(fread(&nbucket, sizeof(nbucket), 1, f), 1u);
  UT_ASSERT_EQ(fread(&nchain, sizeof(nchain), 1, f), 1u);
  UT_ASSERT_EQ(nbucket, 4u);
  UT_ASSERT_EQ(nchain, 4u);

  uint32_t bucket[4], chain[4];
  UT_ASSERT_EQ(fread(bucket, sizeof(uint32_t), 4, f), 4u);
  UT_ASSERT_EQ(fread(chain, sizeof(uint32_t), 4, f), 4u);

  uint32_t b = tcc_yaff_hash("x") % 4;
  UT_ASSERT_EQ(bucket[b], 1u);

  fclose(f);
  unlink(path);
  tcc_free_hash_table(&ht);
  return 0;
}

/* ============================================================================
 * Higher-level: load / resolve / free on a constructed YAFF file
 * ============================================================================ */

/* Build a minimal in-memory YAFF file containing one exported symbol named
 * `symname` at offset `offset` in the CODE section.  Returns a malloc'd buffer
 * that the caller must free; writes the buffer size into *size. */
static uint8_t *ut_build_yaff(const char *objname, const char *symname, uint32_t offset, size_t *size)
{
  YaffHeader h = {0};
  memcpy(h.magic, "YAFF", 4);
  h.alignment = 4;
  h.exported_symbols_amount = 2; /* sentinel at index 0 + one real symbol */

  /* Layout:
   *   YaffHeader
   *   object name (aligned)
   *   exported symbols region [exported_symbols_offset, imported_symbols_lookup_offset)
   *     sentinel YaffSymbolEntry + 4 padding bytes
   *     real YaffSymbolEntry + name (aligned)
   *   lookup table (one u16 per exported symbol)
   *   hash table [nbucket, nchain, bucket[], chain[]]
   */
  size_t objname_len = strlen(objname) + 1;
  size_t aligned_objname_len = (objname_len + h.alignment - 1) & ~(h.alignment - 1);

  size_t sym_name_len = strlen(symname) + 1;
  size_t aligned_sym_name_len = (sym_name_len + h.alignment - 1) & ~(h.alignment - 1);

  size_t entry0_size = sizeof(YaffSymbolEntry) + 4;
  size_t entry1_size = sizeof(YaffSymbolEntry) + aligned_sym_name_len;
  size_t region_size = entry0_size + entry1_size;

  size_t lookup_size = h.exported_symbols_amount * sizeof(uint16_t);

  uint32_t nbucket = 4;
  uint32_t nchain = h.exported_symbols_amount;
  size_t hash_size = 2 * sizeof(uint32_t) + (nbucket + nchain) * sizeof(uint32_t);

  *size = sizeof(YaffHeader) + aligned_objname_len + region_size + lookup_size + hash_size;
  uint8_t *buf = (uint8_t *)tcc_malloc(*size);
  memset(buf, 0, *size);

  uint8_t *p = buf;
  memcpy(p, &h, sizeof(YaffHeader));
  p += sizeof(YaffHeader);

  memcpy(p, objname, objname_len);
  p += aligned_objname_len;

  h.exported_symbols_offset = (uint16_t)(p - buf);

  /* Sentinel entry at region offset 0 (index 0). */
  YaffSymbolEntry *e0 = (YaffSymbolEntry *)p;
  e0->section = 0;
  e0->weak = 0;
  e0->offset = 0;
  p += sizeof(YaffSymbolEntry);
  for (int i = 0; i < 4; i++)
    *p++ = 0;

  /* Real entry at index 1. */
  uint32_t entry1_offset = (uint32_t)(p - (buf + h.exported_symbols_offset));
  YaffSymbolEntry *e1 = (YaffSymbolEntry *)p;
  e1->section = YAFF_SECTION_CODE;
  e1->weak = 0;
  e1->offset = offset;
  p += sizeof(YaffSymbolEntry);
  memcpy(p, symname, sym_name_len);
  p += sym_name_len;
  /* pad to alignment */
  size_t pad = aligned_sym_name_len - sym_name_len;
  for (size_t i = 0; i < pad; i++)
    *p++ = 0;

  h.imported_symbols_lookup_offset = (uint16_t)(p - buf);
  h.exported_symbols_lookup_offset = (uint16_t)(p - buf);

  /* Lookup table: index 0 -> 0, index 1 -> entry1_offset. */
  uint16_t *lookup = (uint16_t *)p;
  lookup[0] = 0;
  lookup[1] = (uint16_t)entry1_offset;
  p += lookup_size;

  h.imported_symbols_hash_table_offset = (uint16_t)(p - buf);
  h.exported_symbols_hash_table_offset = (uint16_t)(p - buf);

  uint32_t *hash = (uint32_t *)p;
  hash[0] = nbucket;
  hash[1] = nchain;
  uint32_t *bucket = hash + 2;
  uint32_t *chain = bucket + nbucket;
  memset(bucket, 0, nbucket * sizeof(uint32_t));
  memset(chain, 0, nchain * sizeof(uint32_t));

  uint32_t sym_hash = tcc_yaff_hash(symname);
  uint32_t b = sym_hash % nbucket;
  bucket[b] = 1;

  /* Patch the header in place. */
  memcpy(buf, &h, sizeof(YaffHeader));

  return buf;
}

/* Minimal dynsymtab_section setup for tcc_yaff_resolve.  We avoid the full
 * tccelf_new() constructor and instead build just enough state for the real
 * set_elf_sym() to intern one symbol. */
static Section *ut_make_dynsymtab(TCCState *s1)
{
  Section *strsec = (Section *)tcc_mallocz(sizeof(Section));
  strsec->s1 = s1;
  strsec->data = (unsigned char *)tcc_malloc(256);
  strsec->data_allocated = 256;
  strsec->data_offset = 1; /* string table starts with a NUL byte */
  strsec->data[0] = 0;

  Section *symsec = (Section *)tcc_mallocz(sizeof(Section));
  symsec->s1 = s1;
  symsec->link = strsec;
  symsec->hash = NULL; /* disable hash-table updates; linear lookup still works */
  symsec->data = (unsigned char *)tcc_malloc(256);
  symsec->data_allocated = 256;
  symsec->data_offset = 0;

  /* Index 0 is the reserved null symbol. */
  section_ptr_add(symsec, sizeof(ElfW(Sym)));

  return symsec;
}

static void ut_free_dynsymtab(Section *symsec)
{
  if (!symsec)
    return;
  tcc_free(symsec->data);
  tcc_free(symsec->link->data);
  tcc_free(symsec->link);
  tcc_free(symsec);
}

UT_TEST(test_load_yaff_rejects_bad_magic)
{
  char buf[64];
  memset(buf, 0, sizeof(buf));
  memcpy(buf, "NOTYAFF", 4);

  char path[] = "/tmp/tccyaff_ut_bad_XXXXXX";
  int fd = mkstemp(path);
  UT_ASSERT(fd >= 0);
  UT_ASSERT_EQ(write(fd, buf, sizeof(buf)), (ssize_t)sizeof(buf));
  lseek(fd, 0, SEEK_SET);

  memset(tcc_state, 0, sizeof(TCCState));
  int rc = tcc_load_yaff(tcc_state, fd, path, 0);
  UT_ASSERT(rc != 0);

  close(fd);
  unlink(path);
  return 0;
}

UT_TEST(test_load_yaff_and_resolve)
{
  size_t size;
  uint8_t *buf = ut_build_yaff("libfoo.yaff", "exported_fn", 0x1234, &size);
  UT_ASSERT(buf != NULL);

  char path[] = "/tmp/tccyaff_ut_load_XXXXXX";
  int fd = mkstemp(path);
  UT_ASSERT(fd >= 0);
  UT_ASSERT_EQ(write(fd, buf, size), (ssize_t)size);
  lseek(fd, 0, SEEK_SET);

  memset(tcc_state, 0, sizeof(TCCState));
  tcc_state->dynsymtab_section = ut_make_dynsymtab(tcc_state);

  int rc = tcc_load_yaff(tcc_state, fd, path, 0);
  UT_ASSERT_EQ(rc, 0);
  UT_ASSERT_EQ(tcc_state->nb_yaff_libs, 1);

  int idx = tcc_yaff_resolve(tcc_state, "exported_fn");
  UT_ASSERT_EQ(idx, 1); /* first interned symbol after the null symbol */

  ElfW(Sym) *sym = &((ElfW(Sym) *)tcc_state->dynsymtab_section->data)[idx];
  UT_ASSERT_EQ(sym->st_value, 0x1234u);
  UT_ASSERT_EQ(ELFW(ST_BIND)(sym->st_info), STB_GLOBAL);
  UT_ASSERT_EQ(ELFW(ST_TYPE)(sym->st_info), STT_FUNC);

  tcc_yaff_libs_free(tcc_state);
  UT_ASSERT(tcc_state->yaff_libs == NULL);
  UT_ASSERT_EQ(tcc_state->nb_yaff_libs, 0);

  ut_free_dynsymtab(tcc_state->dynsymtab_section);
  tcc_state->dynsymtab_section = NULL;

  close(fd);
  unlink(path);
  tcc_free(buf);
  return 0;
}

UT_TEST(test_yaff_resolve_missing_symbol_returns_zero)
{
  size_t size;
  uint8_t *buf = ut_build_yaff("libfoo.yaff", "exported_fn", 0x1234, &size);

  char path[] = "/tmp/tccyaff_ut_miss_XXXXXX";
  int fd = mkstemp(path);
  UT_ASSERT(fd >= 0);
  UT_ASSERT_EQ(write(fd, buf, size), (ssize_t)size);
  lseek(fd, 0, SEEK_SET);

  memset(tcc_state, 0, sizeof(TCCState));
  tcc_state->dynsymtab_section = ut_make_dynsymtab(tcc_state);

  UT_ASSERT_EQ(tcc_load_yaff(tcc_state, fd, path, 0), 0);
  UT_ASSERT_EQ(tcc_yaff_resolve(tcc_state, "no_such_symbol"), 0);

  tcc_yaff_libs_free(tcc_state);
  ut_free_dynsymtab(tcc_state->dynsymtab_section);
  tcc_state->dynsymtab_section = NULL;

  close(fd);
  unlink(path);
  tcc_free(buf);
  return 0;
}

/* ============================================================================
 * Helpers for tcc_output_yaff / tcc_yaff_prepare_init_fini tests
 * ============================================================================ */

static void ut_yaff_reset_state(void)
{
  memset(tcc_state, 0, sizeof(TCCState));
}

static void ut_yaff_init_sections(void)
{
  dynarray_add(&tcc_state->sections, &tcc_state->nb_sections, NULL);
}

static Section *ut_yaff_make_simple_section(const char *name, int sh_type, int sh_flags, uint32_t addr, uint32_t size)
{
  Section *sec = new_section(tcc_state, name, sh_type, sh_flags);
  sec->sh_addr = addr;
  if (size > 0 && sh_type != SHT_NOBITS)
  {
    unsigned char *p = section_ptr_add(sec, size);
    memset(p, 0, size);
  }
  sec->sh_size = size;
  return sec;
}

static void ut_yaff_setup_minimal_output_state(void)
{
  ut_yaff_reset_state();
  ut_yaff_init_sections();

  tcc_state->output_type = TCC_OUTPUT_DYN;
  tcc_state->text_and_data_separation = 1;

  text_section = ut_yaff_make_simple_section(".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0x1000, 16);
  rodata_section = ut_yaff_make_simple_section(".rodata", SHT_PROGBITS, SHF_ALLOC, 0x1100, 8);
  data_section = ut_yaff_make_simple_section(".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 0x1108, 8);
  bss_section = ut_yaff_make_simple_section(".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 0x1110, 0);

  tcc_state->got = ut_yaff_make_simple_section(".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 0x1110, 64);
  tcc_state->plt = ut_yaff_make_simple_section(".plt", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0x1150, 0);

  tcc_state->dynsym = new_symtab(tcc_state, ".dynsym", SHT_DYNSYM, SHF_ALLOC, ".dynstr", ".hash", SHF_ALLOC);
  tcc_state->symtab = new_symtab(tcc_state, ".symtab", SHT_SYMTAB, 0, ".strtab", ".hashtab", SHF_PRIVATE);
  tcc_state->dynsymtab_section =
      new_symtab(tcc_state, ".dynsymtab", SHT_SYMTAB, SHF_PRIVATE | SHF_DYNSYM, ".dynstrtab", ".dynhashtab", SHF_PRIVATE);
}

static void ut_yaff_teardown_output_state(void)
{
  tccelf_delete(tcc_state);
}

static FILE *ut_yaff_open_temp(char *path)
{
  int fd = mkstemp(path);
  if (fd < 0)
  {
    fprintf(stderr, "mkstemp failed\n");
    abort();
  }
  FILE *f = fdopen(fd, "w+b");
  if (!f)
  {
    fprintf(stderr, "fdopen failed\n");
    abort();
  }
  return f;
}

static void ut_yaff_read_header(FILE *f, YaffHeader *h)
{
  fflush(f);
  fseek(f, 0, SEEK_SET);
  if (fread(h, sizeof(YaffHeader), 1, f) != 1u)
  {
    fprintf(stderr, "fread of YaffHeader failed\n");
    abort();
  }
}

/* ============================================================================
 * tcc_output_yaff public entry point
 * ============================================================================ */

UT_TEST(test_output_yaff_rejects_on_errors)
{
  ut_yaff_setup_minimal_output_state();
  tcc_state->nb_errors = 1;

  char path[] = "/tmp/tccyaff_ut_out_err_XXXXXX";
  FILE *f = ut_yaff_open_temp(path);

  UT_ASSERT_EQ(tcc_output_yaff(tcc_state, f, "err.yaff"), -1);

  fclose(f);
  unlink(path);
  ut_yaff_teardown_output_state();
  return 0;
}

UT_TEST(test_output_yaff_minimal_header)
{
  ut_yaff_setup_minimal_output_state();

  char path[] = "/tmp/tccyaff_ut_out_min_XXXXXX";
  FILE *f = ut_yaff_open_temp(path);

  UT_ASSERT_EQ(tcc_output_yaff(tcc_state, f, "minimal.yaff"), 0);

  YaffHeader h;
  ut_yaff_read_header(f, &h);
  UT_ASSERT_EQ(memcmp(h.magic, "YAFF", 4), 0);
  UT_ASSERT_EQ(h.module_type, 2u); /* TCC_OUTPUT_DYN -> 2 */
  UT_ASSERT_EQ(h.alignment, 4u);
  UT_ASSERT_EQ(h.code_length, 16u);
  UT_ASSERT_EQ(h.data_length, 16u); /* rodata 8 + data 8 */
  UT_ASSERT_EQ(h.bss_length, 0u);
  UT_ASSERT_EQ(h.external_libraries_amount, 0u);
  UT_ASSERT_EQ(h.text_and_data_separation, 1u);
  UT_ASSERT_EQ(h.version_major, 0u);
  UT_ASSERT_EQ(h.version_minor, 0u);
  UT_ASSERT_EQ(h.stack_size, 0xFFFFFFFFu);
  UT_ASSERT_EQ(h.heap_size, 0xFFFFFFFFu);

  /* Object name follows the header immediately. */
  char name[32];
  fseek(f, sizeof(YaffHeader), SEEK_SET);
  UT_ASSERT_EQ(fread(name, 1, sizeof("minimal.yaff"), f), sizeof("minimal.yaff"));
  UT_ASSERT_STREQ(name, "minimal.yaff");

  fclose(f);
  unlink(path);
  ut_yaff_teardown_output_state();
  return 0;
}

UT_TEST(test_output_yaff_with_exported_symbol)
{
  ut_yaff_setup_minimal_output_state();

  /* One exported function and one local symbol that must be filtered out. */
  set_elf_sym(tcc_state->dynsym, 0x1004, 1, ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC), STV_DEFAULT, text_section->sh_num,
              "exported_fn");
  set_elf_sym(tcc_state->dynsym, 0x2000, 1, ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT), STV_DEFAULT, data_section->sh_num,
              "local_sym");

  char path[] = "/tmp/tccyaff_ut_out_exp_XXXXXX";
  FILE *f = ut_yaff_open_temp(path);

  UT_ASSERT_EQ(tcc_output_yaff(tcc_state, f, "exported.yaff"), 0);

  YaffHeader h;
  ut_yaff_read_header(f, &h);
  UT_ASSERT_EQ(h.exported_symbols_amount, 2u); /* sentinel + exported_fn */

  fseek(f, h.exported_symbols_offset, SEEK_SET);
  /* Skip sentinel entry (YaffSymbolEntry + 4 padding bytes). */
  fseek(f, (long)(sizeof(YaffSymbolEntry) + 4), SEEK_CUR);

  YaffSymbolEntry e;
  UT_ASSERT_EQ(fread(&e, sizeof(YaffSymbolEntry), 1, f), 1u);
  UT_ASSERT_EQ(e.section, (uint32_t)YAFF_SECTION_CODE);
  UT_ASSERT_EQ(e.weak, 0u);
  UT_ASSERT_EQ(e.offset, 0x1004u);

  char name[32];
  UT_ASSERT_EQ(fread(name, 1, sizeof("exported_fn"), f), sizeof("exported_fn"));
  UT_ASSERT_STREQ(name, "exported_fn");

  fclose(f);
  unlink(path);
  ut_yaff_teardown_output_state();
  return 0;
}

UT_TEST(test_output_yaff_with_imported_symbol)
{
  ut_yaff_setup_minimal_output_state();

  set_elf_sym(tcc_state->dynsym, 0, 1, ELFW(ST_INFO)(STB_GLOBAL, STT_NOTYPE), STV_DEFAULT, SHN_UNDEF, "imported_fn");
  /* Weak imported symbol must be flagged as weak. */
  set_elf_sym(tcc_state->dynsym, 0, 1, ELFW(ST_INFO)(STB_WEAK, STT_NOTYPE), STV_DEFAULT, SHN_UNDEF, "weak_import");

  char path[] = "/tmp/tccyaff_ut_out_imp_XXXXXX";
  FILE *f = ut_yaff_open_temp(path);

  UT_ASSERT_EQ(tcc_output_yaff(tcc_state, f, "imported.yaff"), 0);

  YaffHeader h;
  ut_yaff_read_header(f, &h);
  UT_ASSERT_EQ(h.imported_symbols_amount, 3u); /* sentinel + 2 imports */

  fseek(f, h.imported_symbols_offset, SEEK_SET);
  fseek(f, (long)(sizeof(YaffSymbolEntry) + 4), SEEK_CUR);

  YaffSymbolEntry e;
  UT_ASSERT_EQ(fread(&e, sizeof(YaffSymbolEntry), 1, f), 1u);
  UT_ASSERT_EQ(e.section, 0u);
  UT_ASSERT_EQ(e.weak, 0u);

  char name[32];
  UT_ASSERT_EQ(fread(name, 1, sizeof("imported_fn"), f), sizeof("imported_fn"));
  UT_ASSERT_STREQ(name, "imported_fn");

  /* Verify hash table references the imported symbol. */
  fseek(f, h.imported_symbols_hash_table_offset, SEEK_SET);
  uint32_t nbucket, nchain;
  UT_ASSERT_EQ(fread(&nbucket, sizeof(nbucket), 1, f), 1u);
  UT_ASSERT_EQ(fread(&nchain, sizeof(nchain), 1, f), 1u);
  UT_ASSERT(nbucket > 0);
  UT_ASSERT_EQ(nchain, h.imported_symbols_amount);

  fclose(f);
  unlink(path);
  ut_yaff_teardown_output_state();
  return 0;
}

UT_TEST(test_output_yaff_local_relocation)
{
  ut_yaff_setup_minimal_output_state();

  /* Build .rel.got with one R_RELATIVE relocation at GOT byte offset 8. */
  Section *relgot = new_section(tcc_state, ".rel.got", SHT_REL, SHF_ALLOC);
  relgot->link = tcc_state->dynsym;
  tcc_state->got->reloc = relgot;

  ElfW_Rel rel = {
      .r_offset = tcc_state->got->sh_addr + 8,
      .r_info = ELF32_R_INFO(0, R_ARM_RELATIVE),
  };
  unsigned char *rel_data = section_ptr_add(relgot, sizeof(ElfW_Rel));
  memcpy(rel_data, &rel, sizeof(ElfW_Rel));

  /* GOT slot at offset 8 holds a code address; second word holds STT_FUNC. */
  write32le(tcc_state->got->data + 8, 0x1004);
  write32le(tcc_state->got->data + 8 + PTR_SIZE, STT_FUNC);

  char path[] = "/tmp/tccyaff_ut_out_loc_XXXXXX";
  FILE *f = ut_yaff_open_temp(path);

  UT_ASSERT_EQ(tcc_output_yaff(tcc_state, f, "local.yaff"), 0);

  YaffHeader h;
  ut_yaff_read_header(f, &h);
  UT_ASSERT_EQ(h.local_relocations_amount, 1u);

  fseek(f, h.relocations_offset, SEEK_SET);
  YaffLocalRelocationEntry local;
  UT_ASSERT_EQ(fread(&local, sizeof(YaffLocalRelocationEntry), 1, f), 1u);
  UT_ASSERT_EQ(local.section, (uint32_t)YAFF_SECTION_CODE);
  UT_ASSERT_EQ(local.index, 1u); /* got_offset / 8 */
  UT_ASSERT_EQ(local.target_offset, 0x4u); /* 0x1004 - text base */

  fclose(f);
  unlink(path);
  ut_yaff_teardown_output_state();
  return 0;
}

UT_TEST(test_output_yaff_data_relocation)
{
  ut_yaff_setup_minimal_output_state();

  /* Add a defined symbol in .text so the relocation is not treated as imported. */
  set_elf_sym(tcc_state->symtab, 0x1000, 1, ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC), STV_DEFAULT, text_section->sh_num,
              "target_fn");

  /* .rel.rodata with one R_ARM_ABS32 relocation at rodata offset 0. */
  Section *rel = new_section(tcc_state, ".rel.rodata", SHT_REL, SHF_ALLOC);
  rel->link = tcc_state->symtab;
  rel->sh_info = rodata_section->sh_num;

  ElfW_Rel r = {
      .r_offset = rodata_section->sh_addr,
      .r_info = ELF32_R_INFO(1, R_ARM_ABS32),
  };
  unsigned char *rel_data = section_ptr_add(rel, sizeof(ElfW_Rel));
  memcpy(rel_data, &r, sizeof(ElfW_Rel));

  /* The rodata word at offset 0 holds a code address -> towards_code. */
  write32le(rodata_section->data, 0x1000);

  char path[] = "/tmp/tccyaff_ut_out_data_XXXXXX";
  FILE *f = ut_yaff_open_temp(path);

  UT_ASSERT_EQ(tcc_output_yaff(tcc_state, f, "data.yaff"), 0);

  YaffHeader h;
  ut_yaff_read_header(f, &h);
  UT_ASSERT_EQ(h.data_relocations_amount, 1u);

  /* Skip symbol-table and local relocation blocks (both empty here). */
  fseek(f, h.relocations_offset, SEEK_SET);

  YaffDataRelocationEntry entry;
  UT_ASSERT_EQ(fread(&entry, sizeof(YaffDataRelocationEntry), 1, f), 1u);
  UT_ASSERT_EQ(entry.section, (uint32_t)YAFF_SECTION_CODE);
  UT_ASSERT_EQ(entry.to, 0u);

  fclose(f);
  unlink(path);
  ut_yaff_teardown_output_state();
  return 0;
}

UT_TEST(test_output_yaff_symbol_table_relocation)
{
  ut_yaff_setup_minimal_output_state();

  /* One imported dynamic symbol. */
  int sym_idx = set_elf_sym(tcc_state->dynsym, 0, 1, ELFW(ST_INFO)(STB_GLOBAL, STT_NOTYPE), STV_DEFAULT, SHN_UNDEF,
                            "imported_fn");
  UT_ASSERT_EQ(sym_idx, 1);

  /* .rel.dyn linked to dynsym, holding one GLOB_DAT relocation in the GOT. */
  Section *rel = new_section(tcc_state, ".rel.dyn", SHT_REL, SHF_ALLOC);
  rel->link = tcc_state->dynsym;
  rel->sh_info = tcc_state->got->sh_num;

  uint32_t got_slot_offset = 16;
  ElfW_Rel r = {
      .r_offset = tcc_state->got->sh_addr + got_slot_offset,
      .r_info = ELF32_R_INFO(sym_idx, R_ARM_GLOB_DAT),
  };
  unsigned char *rel_data = section_ptr_add(rel, sizeof(ElfW_Rel));
  memcpy(rel_data, &r, sizeof(ElfW_Rel));

  char path[] = "/tmp/tccyaff_ut_out_sym_XXXXXX";
  FILE *f = ut_yaff_open_temp(path);

  UT_ASSERT_EQ(tcc_output_yaff(tcc_state, f, "sym.yaff"), 0);

  YaffHeader h;
  ut_yaff_read_header(f, &h);
  UT_ASSERT_EQ(h.symbol_table_relocations_amount, 1u);

  fseek(f, h.relocations_offset, SEEK_SET);

  YaffSymbolTableRelocationEntry entry;
  UT_ASSERT_EQ(fread(&entry, sizeof(YaffSymbolTableRelocationEntry), 1, f), 1u);
  UT_ASSERT_EQ(entry.is_exported_symbol, 0u);
  UT_ASSERT_EQ(entry.index, got_slot_offset / 8);
  UT_ASSERT_EQ(entry.function_pointer, 0u);
  UT_ASSERT_EQ(entry.plt_call, 0u);
  UT_ASSERT_EQ(entry.symbol_index, 1u); /* first imported symbol */

  fclose(f);
  unlink(path);
  ut_yaff_teardown_output_state();
  return 0;
}

UT_TEST(test_output_yaff_exported_hidden_symbol_filtered)
{
  ut_yaff_setup_minimal_output_state();

  /* A global symbol with hidden visibility must not appear in the export table. */
  set_elf_sym(tcc_state->dynsym, 0x1004, 1, ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC), STV_HIDDEN, text_section->sh_num,
              "hidden_fn");

  char path[] = "/tmp/tccyaff_ut_out_hid_XXXXXX";
  FILE *f = ut_yaff_open_temp(path);

  UT_ASSERT_EQ(tcc_output_yaff(tcc_state, f, "hidden.yaff"), 0);

  YaffHeader h;
  ut_yaff_read_header(f, &h);
  UT_ASSERT_EQ(h.exported_symbols_amount, 1u); /* sentinel only */

  fclose(f);
  unlink(path);
  ut_yaff_teardown_output_state();
  return 0;
}

/* ============================================================================
 * tcc_yaff_prepare_init_fini
 * ============================================================================ */

UT_TEST(test_yaff_prepare_init_fini_merge)
{
  ut_yaff_setup_minimal_output_state();

  Section *ia = new_section(tcc_state, ".init_array", SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE);
  Section *fa = new_section(tcc_state, ".fini_array", SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE);

  /* .init_array with two function pointers. */
  uint32_t init_ptrs[2] = {0x1000, 0x1004};
  unsigned char *init_data = section_ptr_add(ia, 2 * PTR_SIZE);
  write32le(init_data, init_ptrs[0]);
  write32le(init_data + PTR_SIZE, init_ptrs[1]);

  /* .fini_array with one function pointer. */
  unsigned char *fini_data = section_ptr_add(fa, PTR_SIZE);
  write32le(fini_data, 0x1008);

  /* Relocation for the first init_array slot. */
  Section *rel_ia = new_section(tcc_state, ".rel.init_array", SHT_REL, SHF_ALLOC);
  rel_ia->link = tcc_state->symtab;
  ia->reloc = rel_ia;
  ElfW_Rel r = {
      .r_offset = 0,
      .r_info = ELF32_R_INFO(0, R_ARM_ABS32),
  };
  unsigned char *rel_data = section_ptr_add(rel_ia, sizeof(ElfW_Rel));
  memcpy(rel_data, &r, sizeof(ElfW_Rel));

  uint32_t data_before = data_section->data_offset;

  tcc_yaff_prepare_init_fini(tcc_state);

  /* data_section should now contain:
   *   [init_count (4)] [fini_count (4)] [init0 (4)] [init1 (4)] [fini0 (4)] */
  UT_ASSERT_EQ(data_section->data_offset, data_before + 2 * sizeof(uint32_t) + 2 * PTR_SIZE + PTR_SIZE);

  uint32_t counts[2];
  memcpy(counts, data_section->data + data_before, sizeof(counts));
  UT_ASSERT_EQ(counts[0], 2u);
  UT_ASSERT_EQ(counts[1], 1u);

  UT_ASSERT_EQ(read32le(data_section->data + data_before + 2 * sizeof(uint32_t)), 0x1000u);
  UT_ASSERT_EQ(read32le(data_section->data + data_before + 2 * sizeof(uint32_t) + PTR_SIZE), 0x1004u);
  UT_ASSERT_EQ(read32le(data_section->data + data_before + 2 * sizeof(uint32_t) + 2 * PTR_SIZE), 0x1008u);

  /* A relocation was copied to .data for the first init slot. */
  UT_ASSERT(data_section->reloc != NULL);
  UT_ASSERT_EQ(data_section->reloc->data_offset, sizeof(ElfW_Rel));

  /* __yaff_initfini symbol was defined in symtab. */
  int found = 0;
  int nb_syms = tcc_state->symtab->data_offset / sizeof(ElfW(Sym));
  for (int i = 1; i < nb_syms; ++i)
  {
    ElfW(Sym) *sym = &((ElfW(Sym) *)tcc_state->symtab->data)[i];
    const char *sname = (char *)tcc_state->symtab->link->data + sym->st_name;
    if (!strcmp(sname, "__yaff_initfini"))
    {
      found = 1;
      UT_ASSERT_EQ(ELFW(ST_BIND)(sym->st_info), (uint32_t)STB_LOCAL);
      UT_ASSERT_EQ(sym->st_shndx, data_section->sh_num);
    }
  }
  UT_ASSERT(found);

  /* Original init/fini sections are suppressed. */
  UT_ASSERT_EQ(ia->sh_type, (uint32_t)SHT_NULL);
  UT_ASSERT_EQ(fa->sh_type, (uint32_t)SHT_NULL);

  ut_yaff_teardown_output_state();
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(tccyaff)
{
  UT_RUN(test_yaff_hash_empty);
  UT_RUN(test_yaff_hash_simple_strings);
  UT_RUN(test_yaff_align_power_of_two);
  UT_RUN(test_parse_object_name);
  UT_RUN(test_get_offset_to_imported_libraries);

  UT_RUN(test_hash_table_allocate_zeroes);
  UT_RUN(test_hash_table_add_single);
  UT_RUN(test_hash_table_add_collision_chains);
  UT_RUN(test_hash_table_write_and_readback);

  UT_RUN(test_load_yaff_rejects_bad_magic);
  UT_RUN(test_load_yaff_and_resolve);
  UT_RUN(test_yaff_resolve_missing_symbol_returns_zero);

  UT_RUN(test_output_yaff_rejects_on_errors);
  UT_RUN(test_output_yaff_minimal_header);
  UT_RUN(test_output_yaff_with_exported_symbol);
  UT_RUN(test_output_yaff_with_imported_symbol);
  UT_RUN(test_output_yaff_local_relocation);
  UT_RUN(test_output_yaff_data_relocation);
  UT_RUN(test_output_yaff_symbol_table_relocation);
  UT_RUN(test_output_yaff_exported_hidden_symbol_filtered);

  UT_RUN(test_yaff_prepare_init_fini_merge);
}
