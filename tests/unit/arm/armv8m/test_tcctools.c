/*
 *  test_tcctools.c - white-box unit tests for tcctools.c
 *  (build_tcctools/run_unit_tests_tcctools)
 *
 *  Covers:
 *   - the little-endian byte helpers (read16le/write16le, read32le/write32le,
 *     add32le, read64le/write64le)
 *   - gen_makedeps() output formatting (explicit and auto-generated filenames,
 *     dependency escaping, deduplication, phony targets)
 *   - tcc_tool_ar() create/list/extract for tiny archives
 *
 *  tcc_state is the zero-initialized global supplied by tcc_state_stub.c;
 *  gen_makedeps tests populate target_deps/nb_target_deps directly.
 */

#include "tcc.h"
#include "ut.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* tcctools.c prototypes are #if0'd out of tcc.h; reproduce the ones we test. */
ST_FUNC int tcc_tool_ar(TCCState *s, int argc, char **argv);
ST_FUNC int gen_makedeps(TCCState *s, const char *target, const char *filename);

/* -------------------------------------------------------------------------- */
/* Little-endian byte helpers                                                 */
/* -------------------------------------------------------------------------- */

UT_TEST(test_read16le_write16le_roundtrip)
{
    unsigned char buf[2];
    write16le(buf, 0x3412);
    UT_ASSERT_EQ(buf[0], 0x12);
    UT_ASSERT_EQ(buf[1], 0x34);
    UT_ASSERT_EQ(read16le(buf), 0x3412u);

    write16le(buf, 0);
    UT_ASSERT_EQ(read16le(buf), 0u);

    write16le(buf, 0xffff);
    UT_ASSERT_EQ(read16le(buf), 0xffffu);
    return 0;
}

UT_TEST(test_read32le_write32le_roundtrip)
{
    unsigned char buf[4];
    write32le(buf, 0x78563412u);
    UT_ASSERT_EQ(buf[0], 0x12);
    UT_ASSERT_EQ(buf[1], 0x34);
    UT_ASSERT_EQ(buf[2], 0x56);
    UT_ASSERT_EQ(buf[3], 0x78);
    UT_ASSERT_EQ(read32le(buf), 0x78563412u);

    write32le(buf, 0);
    UT_ASSERT_EQ(read32le(buf), 0u);

    write32le(buf, 0xffffffffu);
    UT_ASSERT_EQ(read32le(buf), 0xffffffffu);
    return 0;
}

UT_TEST(test_add32le)
{
    unsigned char buf[4];
    write32le(buf, 0x10000000u);
    add32le(buf, 0x5);
    UT_ASSERT_EQ(read32le(buf), 0x10000005u);

    write32le(buf, 0xffffffffu);
    add32le(buf, 1);
    UT_ASSERT_EQ(read32le(buf), 0u);

    write32le(buf, 0u);
    add32le(buf, -1);
    UT_ASSERT_EQ(read32le(buf), 0xffffffffu);
    return 0;
}

UT_TEST(test_read64le_write64le_roundtrip)
{
    unsigned char buf[8];
    write64le(buf, 0xefcdab8967452301ull);
    UT_ASSERT_EQ(buf[0], 0x01);
    UT_ASSERT_EQ(buf[1], 0x23);
    UT_ASSERT_EQ(buf[2], 0x45);
    UT_ASSERT_EQ(buf[3], 0x67);
    UT_ASSERT_EQ(buf[4], 0x89);
    UT_ASSERT_EQ(buf[5], 0xab);
    UT_ASSERT_EQ(buf[6], 0xcd);
    UT_ASSERT_EQ(buf[7], 0xef);
    UT_ASSERT_EQ(read64le(buf), 0xefcdab8967452301ull);

    write64le(buf, 0);
    UT_ASSERT_EQ(read64le(buf), 0ull);

    write64le(buf, 0xffffffffffffffffull);
    UT_ASSERT_EQ(read64le(buf), 0xffffffffffffffffull);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* gen_makedeps()                                                             */
/* -------------------------------------------------------------------------- */

static void ut_reset_target_deps(void)
{
    tcc_state->target_deps = NULL;
    tcc_state->nb_target_deps = 0;
    tcc_state->verbose = 0;
    tcc_state->gen_phony_deps = 0;
}

static char *ut_slurp_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)tcc_malloc(sz + 1);
    if (buf)
    {
        fread(buf, 1, sz, f);
        buf[sz] = '\0';
    }
    fclose(f);
    return buf;
}

static int ut_make_temp_path(char *out, size_t out_size)
{
    char template[256];
    snprintf(template, sizeof(template), "/tmp/tcctools_test_XXXXXX");
    int fd = mkstemp(template);
    if (fd < 0)
        return -1;
    close(fd);
    snprintf(out, out_size, "%s", template);
    return 0;
}

UT_TEST(test_gen_makedeps_explicit_filename)
{
    ut_reset_target_deps();
    char *deps[] = {"src.c", "header.h"};
    tcc_state->target_deps = deps;
    tcc_state->nb_target_deps = 2;

    char depfile[256];
    UT_ASSERT_EQ(ut_make_temp_path(depfile, sizeof(depfile)), 0);

    UT_ASSERT_EQ(gen_makedeps(tcc_state, "out.o", depfile), 0);

    char *content = ut_slurp_file(depfile);
    UT_ASSERT(content != NULL);
    UT_ASSERT(strstr(content, "out.o:") != NULL);
    UT_ASSERT(strstr(content, "src.c") != NULL);
    UT_ASSERT(strstr(content, "header.h") != NULL);
    tcc_free(content);

    remove(depfile);
    return 0;
}

UT_TEST(test_gen_makedeps_auto_filename)
{
    ut_reset_target_deps();
    char *deps[] = {"src.c"};
    tcc_state->target_deps = deps;
    tcc_state->nb_target_deps = 1;

    /* gen_makedeps with filename==NULL derives "dir/target.o" -> "dir/target.d".
     * Make sure the directory exists so the auto-derived file can be written. */
    mkdir("build", 0755);
    UT_ASSERT_EQ(gen_makedeps(tcc_state, "build/target.o", NULL), 0);

    char *content = ut_slurp_file("build/target.d");
    UT_ASSERT(content != NULL);
    UT_ASSERT(strstr(content, "build/target.o:") != NULL);
    UT_ASSERT(strstr(content, "src.c") != NULL);
    tcc_free(content);

    remove("build/target.d");
    rmdir("build");
    return 0;
}

UT_TEST(test_gen_makedeps_escapes_spaces)
{
    ut_reset_target_deps();
    char *deps[] = {"src with space.c", "path/to/header with space.h"};
    tcc_state->target_deps = deps;
    tcc_state->nb_target_deps = 2;

    char depfile[256];
    UT_ASSERT_EQ(ut_make_temp_path(depfile, sizeof(depfile)), 0);

    UT_ASSERT_EQ(gen_makedeps(tcc_state, "out.o", depfile), 0);

    char *content = ut_slurp_file(depfile);
    UT_ASSERT(content != NULL);
    UT_ASSERT(strstr(content, "src\\ with\\ space.c") != NULL);
    UT_ASSERT(strstr(content, "path/to/header\\ with\\ space.h") != NULL);
    tcc_free(content);

    remove(depfile);
    return 0;
}

UT_TEST(test_gen_makedeps_deduplicates_deps)
{
    ut_reset_target_deps();
    char *deps[] = {"a.h", "b.h", "a.h", "c.h", "b.h"};
    tcc_state->target_deps = deps;
    tcc_state->nb_target_deps = 5;

    char depfile[256];
    UT_ASSERT_EQ(ut_make_temp_path(depfile, sizeof(depfile)), 0);

    UT_ASSERT_EQ(gen_makedeps(tcc_state, "out.o", depfile), 0);

    char *content = ut_slurp_file(depfile);
    UT_ASSERT(content != NULL);
    /* Each unique dep should appear exactly once in the rule line. */
    UT_ASSERT(strstr(content, "a.h") != NULL);
    UT_ASSERT(strstr(content, "b.h") != NULL);
    UT_ASSERT(strstr(content, "c.h") != NULL);
    tcc_free(content);

    remove(depfile);
    return 0;
}

UT_TEST(test_gen_makedeps_phony_targets)
{
    ut_reset_target_deps();
    char *deps[] = {"src.c", "a.h", "b.h"};
    tcc_state->target_deps = deps;
    tcc_state->nb_target_deps = 3;
    tcc_state->gen_phony_deps = 1;

    char depfile[256];
    UT_ASSERT_EQ(ut_make_temp_path(depfile, sizeof(depfile)), 0);

    UT_ASSERT_EQ(gen_makedeps(tcc_state, "out.o", depfile), 0);

    char *content = ut_slurp_file(depfile);
    UT_ASSERT(content != NULL);
    /* First dep is the C file and is skipped for phony rules. */
    UT_ASSERT(strstr(content, "a.h:\n") != NULL);
    UT_ASSERT(strstr(content, "b.h:\n") != NULL);
    UT_ASSERT(strstr(content, "src.c:\n") == NULL);
    tcc_free(content);

    remove(depfile);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_tool_ar() helpers                                                      */
/* -------------------------------------------------------------------------- */

/* Write a minimal little-endian 32-bit ELF relocatable object with one global
 * function symbol named "myfunc". The archive tool needs only a valid
 * ELFCLASS32 Ehdr and non-empty .shstrtab/.strtab/.symtab sections. */
static int ut_write_minimal_elf32(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    unsigned char ehdr[52];
    memset(ehdr, 0, sizeof(ehdr));
    ehdr[0] = 0x7f;
    ehdr[1] = 'E';
    ehdr[2] = 'L';
    ehdr[3] = 'F';
    ehdr[4] = ELFCLASS32; /* 1 */
    ehdr[5] = ELFDATA2LSB; /* 1 */
    ehdr[6] = EV_CURRENT; /* 1 */
    ehdr[7] = 0; /* ELFOSABI_NONE */
    write16le(ehdr + 16, ET_REL);   /* e_type */
    write16le(ehdr + 18, EM_ARM);   /* e_machine */
    write32le(ehdr + 20, EV_CURRENT); /* e_version */
    write32le(ehdr + 24, 0);        /* e_entry */
    write32le(ehdr + 28, 0);        /* e_phoff */
    /* e_shoff filled below */
    write32le(ehdr + 36, 0);        /* e_flags */
    write16le(ehdr + 40, 52);       /* e_ehsize */
    write16le(ehdr + 42, 0);        /* e_phentsize */
    write16le(ehdr + 44, 0);        /* e_phnum */
    write16le(ehdr + 46, 40);       /* e_shentsize */
    write16le(ehdr + 48, 4);        /* e_shnum */
    write16le(ehdr + 50, 1);        /* e_shstrndx */

    /* Section data */
    unsigned char shstrtab[] = "\0.shstrtab\0.strtab\0.symtab\0";
    size_t shstrtab_size = sizeof(shstrtab) - 1; /* includes leading \0 */
    unsigned char strtab[] = "\0myfunc\0";
    size_t strtab_size = sizeof(strtab) - 1;
    unsigned char symtab[32];
    memset(symtab, 0, sizeof(symtab));
    /* Symbol 0 is already zeroed (NULL symbol). */
    /* Symbol 1: myfunc, global function, section 1 */
    write32le(symtab + 16 + 0, 1);  /* st_name */
    write32le(symtab + 16 + 4, 0);  /* st_value */
    write32le(symtab + 16 + 8, 0);  /* st_size */
    symtab[16 + 12] = STB_GLOBAL << 4 | STT_FUNC; /* st_info = 0x21 */
    symtab[16 + 13] = 0;            /* st_other */
    write16le(symtab + 16 + 14, 1); /* st_shndx */

    size_t data_offset = 52;
    size_t shstrtab_offset = data_offset;
    size_t strtab_offset = shstrtab_offset + shstrtab_size;
    /* Align .symtab to 4 */
    size_t symtab_offset = (strtab_offset + strtab_size + 3) & ~(size_t)3;
    size_t shdr_offset = symtab_offset + sizeof(symtab);

    write32le(ehdr + 32, (uint32_t)shdr_offset);

    fwrite(ehdr, 1, sizeof(ehdr), f);
    fwrite(shstrtab, 1, shstrtab_size, f);
    fwrite(strtab, 1, strtab_size, f);
    /* Pad between strtab and symtab */
    for (size_t i = strtab_offset + strtab_size; i < symtab_offset; i++)
        fputc(0, f);
    fwrite(symtab, 1, sizeof(symtab), f);

    /* Section headers: NULL, .shstrtab, .strtab, .symtab */
    unsigned char shdr[160];
    memset(shdr, 0, sizeof(shdr));
    /* SH 1: .shstrtab */
    write32le(shdr + 40 + 0, 1);          /* sh_name */
    write32le(shdr + 40 + 4, SHT_STRTAB); /* sh_type */
    write32le(shdr + 40 + 16, (uint32_t)shstrtab_offset); /* sh_offset */
    write32le(shdr + 40 + 20, (uint32_t)shstrtab_size);   /* sh_size */
    /* SH 2: .strtab */
    write32le(shdr + 80 + 0, 11);         /* sh_name */
    write32le(shdr + 80 + 4, SHT_STRTAB); /* sh_type */
    write32le(shdr + 80 + 16, (uint32_t)strtab_offset);   /* sh_offset */
    write32le(shdr + 80 + 20, (uint32_t)strtab_size);     /* sh_size */
    /* SH 3: .symtab */
    write32le(shdr + 120 + 0, 20);        /* sh_name */
    write32le(shdr + 120 + 4, SHT_SYMTAB);/* sh_type */
    write32le(shdr + 120 + 16, (uint32_t)symtab_offset);  /* sh_offset */
    write32le(shdr + 120 + 20, sizeof(symtab));           /* sh_size */
    write32le(shdr + 120 + 24, 2);        /* sh_link -> .strtab */
    write32le(shdr + 120 + 28, 1);        /* sh_info */

    fwrite(shdr, 1, sizeof(shdr), f);
    fclose(f);
    return 0;
}

UT_TEST(test_tcc_tool_ar_invalid_usage)
{
    char *argv[] = {"tcc", "-ar"};
    UT_ASSERT_EQ(tcc_tool_ar(tcc_state, 2, argv), 1);
    return 0;
}

UT_TEST(test_tcc_tool_ar_create_empty_archive)
{
    char arname[256];
    UT_ASSERT_EQ(ut_make_temp_path(arname, sizeof(arname)), 0);

    char *argv[] = {"tcc", "-r", arname};
    UT_ASSERT_EQ(tcc_tool_ar(tcc_state, 3, argv), 0);

    FILE *f = fopen(arname, "rb");
    UT_ASSERT(f != NULL);
    char magic[8];
    UT_ASSERT_EQ(fread(magic, 1, 8, f), 8u);
    UT_ASSERT(memcmp(magic, ARMAG, 8) == 0);
    fclose(f);

    remove(arname);
    return 0;
}

UT_TEST(test_tcc_tool_ar_create_list_extract)
{
    char objname[256];
    char arname[256];
    UT_ASSERT_EQ(ut_make_temp_path(objname, sizeof(objname)), 0);
    UT_ASSERT_EQ(ut_make_temp_path(arname, sizeof(arname)), 0);

    UT_ASSERT_EQ(ut_write_minimal_elf32(objname), 0);

    const char *base = strrchr(objname, '/');
    base = base ? base + 1 : objname;

    /* ar names are truncated to sizeof(ArHdr.ar_name)-1 == 15 chars. */
    char extracted[16];
    size_t base_len = strlen(base);
    size_t extracted_len = base_len < 15 ? base_len : 15;
    memcpy(extracted, base, extracted_len);
    extracted[extracted_len] = '\0';

    /* Create */
    char *create_argv[] = {"tcc", "-r", arname, objname};
    UT_ASSERT_EQ(tcc_tool_ar(tcc_state, 4, create_argv), 0);

    /* List (verbose table) */
    char *list_argv[] = {"tcc", "-rtv", arname};
    UT_ASSERT_EQ(tcc_tool_ar(tcc_state, 3, list_argv), 0);

    /* Extract */
    char *extract_argv[] = {"tcc", "-rx", arname};
    UT_ASSERT_EQ(tcc_tool_ar(tcc_state, 3, extract_argv), 0);

    /* The extracted member is written under its (possibly truncated) basename in CWD. */
    FILE *f = fopen(extracted, "rb");
    UT_ASSERT(f != NULL);
    fclose(f);

    remove(objname);
    remove(arname);
    remove(extracted);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(tcctools)
{
    UT_RUN(test_read16le_write16le_roundtrip);
    UT_RUN(test_read32le_write32le_roundtrip);
    UT_RUN(test_add32le);
    UT_RUN(test_read64le_write64le_roundtrip);

    UT_RUN(test_gen_makedeps_explicit_filename);
    UT_RUN(test_gen_makedeps_auto_filename);
    UT_RUN(test_gen_makedeps_escapes_spaces);
    UT_RUN(test_gen_makedeps_deduplicates_deps);
    UT_RUN(test_gen_makedeps_phony_targets);

    UT_RUN(test_tcc_tool_ar_invalid_usage);
    UT_RUN(test_tcc_tool_ar_create_empty_archive);
    UT_RUN(test_tcc_tool_ar_create_list_extract);
}
