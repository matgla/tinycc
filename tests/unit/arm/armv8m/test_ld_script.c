/*
 *  test_ld_script.c - suite for tccld.c (GNU-ld-style linker script parser)
 *
 *  tccld.c has zero prior unit-test coverage. It's already compiled into
 *  this binary for real (see UT_MODULE_SRCS in Makefile) alongside
 *  arm-link.c, but nothing exercised its entry points before this file, so
 *  -ffunction-sections/--gc-sections silently dropped its object code
 *  (including an undefined reference to pstrcpy(), see below).
 *
 *  Covers:
 *    - MEMORY {} region parsing: attributes, ORIGIN/LENGTH (incl. K/M/G
 *      suffixes and org/o/len/l abbreviations), the LD_MAX_MEMORY_REGIONS
 *      guard, ld_script_find_memory_region().
 *    - PHDRS {} program-header parsing: recognized PT_* type keywords, and
 *      what happens to an unrecognized one.
 *    - ENTRY(sym), including the "name too long" error path.
 *    - The full expression-precedence chain (mul/div/mod, add/sub, shift,
 *      and, xor, or), parens, hex/octal literals, unary ~/-, div-by-zero,
 *      and the ALIGN/ORIGIN/LENGTH/DEFINED/LOADADDR builtins -- all
 *      exercised indirectly via symbol-assignment expressions inside a
 *      SECTIONS {} block (the only place ld_parse_expr's static callees are
 *      reachable from the public API).
 *    - SECTIONS {} output-section parsing: dotted vs bare names, section
 *      patterns (incl. multi-name lists and KEEP()), PROVIDE/PROVIDE_HIDDEN,
 *      memory-region/AT-load-region/phdr association, ld_script_find_output_section().
 *    - ld_section_matches_pattern(): exact, '?', trailing '*', mid-string
 *      '*' backtracking, and the "*" vs empty-string edge case.
 *    - ld_section_should_keep() incl. its explicit NULL-safety check.
 *    - ld_script_find_or_create_symbol()'s find-vs-create identity contract.
 *    - ld_script_add_standard_symbols().
 *    - Malformed/edge input: empty script, whitespace/comments-only script,
 *      an unterminated MEMORY {} block, and a top-level token stream that
 *      contains only stray punctuation.
 *    - ld_script_dump() smoke test (just confirms it doesn't crash).
 *
 *  GENUINE DEFECTS FOUND (see docs/bugs.md write-ups drafted in the task
 *  report -- NOT fixed here per task instructions; each is pinned below as
 *  a regression test documenting the CURRENT, buggy behavior):
 *
 *    BUG A (test_bug_location_counter_dot_is_treated_as_phantom_symbol):
 *      ld_next_token()'s identifier scanner lists '.' as a valid
 *      identifier-*start* character (`isalpha(c) || c=='_' || c=='.' ...`),
 *      so a bare "." in the source is ALWAYS lexed as an LDTOK_NAME token
 *      with tok_buf == ".", never as the raw punctuation value '.' (46).
 *      Every `if (p->tok == '.')` check in tccld.c (ld_parse_primary's
 *      location-counter read, ld_parse_sections' and
 *      ld_parse_output_section_contents' location-counter *assignment*
 *      handling) is therefore unreachable dead code. In practice ". = expr;"
 *      falls through to the generic "symbol assignment" path and silently
 *      creates/updates a symbol literally named "." instead of updating
 *      LDScript.location_counter -- which never advances from its initial
 *      value via script content at all. This breaks the location-counter
 *      feature that's central to real linker scripts (address assignment,
 *      "_end = .;"-style epilogue symbols, ALIGN-relative-to-"." idioms).
 *
 *    BUG A-mul (test_bug_expr_multiplication_operator_never_applies): the
 *      exact same root cause (the identifier-start character class in
 *      ld_next_token() is too permissive) also swallows a standalone '*'
 *      operator into an LDTOK_NAME token instead of the raw punctuation
 *      value '*' (42). ld_parse_mul()'s `while (p->tok == '*' ...)` check
 *      can therefore never fire: "X * Y" always silently evaluates to just
 *      X, and the unconsumed "*" (and whatever follows it) gets picked up
 *      one level out and misparsed as a new top-level SECTIONS item.
 *

 *    BUG B (test_sections_output_section_dotted_with_patterns_and_keep):
 *      ld_parse_section_pattern() unconditionally calls ld_add_pattern()
 *      once *before* parsing the actual glob names inside the parens
 *      (apparently meant to eventually capture the leading file-pattern,
 *      e.g. the "*" in "*(.text*)"), but never writes anything into that
 *      pattern's `.pattern` field. Every single "*(...)"/"NAME(...)"/
 *      "KEEP(...)" occurrence in a SECTIONS output-section body therefore
 *      leaves one permanent bogus LDSectionPattern entry with pattern=="",
 *      type==LD_PAT_GLOB, and keep set to whatever the call passed in --
 *      inflating nb_patterns and polluting ld_script_dump() output.
 *
 *    BUG C (test_bug_memory_invert_attribute_causes_phantom_regions):
 *      ld_expect() does not advance the token position when it reports a
 *      mismatch, and essentially every caller in tccld.c discards its
 *      return value. So once a MEMORY {} region's attribute string uses the
 *      (explicitly scaffolded-for, per the '!' case comment in
 *      ld_parse_memory_attributes) "!rwx"-style invert prefix -- which
 *      cannot lex as part of an identifier token at all, since '!' is
 *      absent from both the identifier-start and identifier-continuation
 *      character sets -- the parser gets stuck re-reporting the same
 *      mismatch and falls into the generic "unrecognized token, skip one
 *      and keep looping" fallback in ld_parse_memory's outer loop, which
 *      then misinterprets the leftover stray tokens ("rx", "ORIGIN",
 *      "LENGTH", ...) as brand-new memory-region names. The net result is
 *      silent data corruption (phantom regions with all-zero fields) with
 *      an overall ld_script_parse_string() return code of 0 (success) --
 *      not a crash, and not a reported error either.
 *
 *    BUG D (test_bug_sections_standard_region_at_phdr_order_drops_phdr):
 *      ld_parse_sections()'s per-output-section suffix-clause parsing
 *      checks '>' (memory region), then ':' (phdr), then "AT" (load
 *      region) -- in that fixed order, exactly once each. Real-world/GNU-ld
 *      scripts conventionally write "> REGION AT > LMA_REGION :PHDR" (AT
 *      *before* the phdr tag); with that ordering this parser's ':' check
 *      has already run (and found "AT", not ':', so it does nothing) by the
 *      time "AT > LMA_REGION" is consumed, and the trailing ":PHDR" is
 *      never looked at again -- os->phdr_idx silently stays -1, no error
 *      reported. Only the non-standard "> REGION :PHDR AT > LMA_REGION"
 *      order (phdr tag before AT) is actually recognized.
 */

#define _POSIX_C_SOURCE 200809L

#include "tccld.h"
#include "tcc.h"

#include "ut.h"

#include <unistd.h>

/* tccld.c is compiled without USING_GLOBALS in this binary (its functions
 * take an explicit TCCState *s1 parameter and reference it via the
 * TCC_SET_STATE(fn) = (tcc_enter_state(s1), fn) expansion of
 * tcc_error_noabort()). tcc_enter_state()/_tcc_error_noabort() are already
 * provided (for real) by test_arm_link.c, which is linked into this same
 * binary -- see that file's HARNESS NOTES for why. pstrcpy() is NOT
 * currently linked anywhere in this binary: it's defined for real only in
 * libtcc.c, which is compile-only here (UT_COVERAGE_ONLY_SRCS). Nothing
 * before this suite ever called into tccld.c's real entry points, so with
 * -ffunction-sections/--gc-sections the undefined reference to pstrcpy was
 * silently dropped along with the never-pulled-in object section.
 * Exercising ld_script_parse_string for real requires it, so provide the
 * verbatim algorithm from libtcc.c's pstrcpy() here (same pattern
 * test_arm_link.c already uses for write16le/add32le/get_sym_attr). */
char *pstrcpy(char *buf, size_t buf_size, const char *s)
{
  char *q, *q_end;
  int c;

  if (buf_size > 0)
  {
    q = buf;
    q_end = buf + buf_size - 1;
    while (q < q_end)
    {
      c = *s++;
      if (c == '\0')
        break;
      *q++ = c;
    }
    *q = '\0';
  }
  return buf;
}

/* Silences ld_script_dump()'s printf() traffic (stdout) for the smoke test
 * so it doesn't spam the unit-test log; mirrors test_tccdebug.c's
 * stderr-capture helper, just discarding to /dev/null instead of a buffer. */
static void ut_ld_script_dump_quiet(LDScript *ld)
{
  int saved_fd;
  FILE *devnull;

  fflush(stdout);
  saved_fd = dup(fileno(stdout));
  devnull = fopen("/dev/null", "w");
  if (devnull)
    dup2(fileno(devnull), fileno(stdout));

  ld_script_dump(ld);

  fflush(stdout);
  if (devnull)
  {
    dup2(saved_fd, fileno(stdout));
    fclose(devnull);
  }
  if (saved_fd >= 0)
    close(saved_fd);
}

/* ------------------------------------------------------------------ */
/* MEMORY {}                                                            */
/* ------------------------------------------------------------------ */

UT_TEST(test_memory_single_region_basic)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_memory_regions, 1);
  UT_ASSERT_STREQ(ld.memory_regions[0].name, "FLASH");
  UT_ASSERT_EQ(ld.memory_regions[0].attributes, LD_MEM_READ | LD_MEM_EXEC);
  UT_ASSERT_EQ(ld.memory_regions[0].origin, 0x08000000u);
  UT_ASSERT_EQ(ld.memory_regions[0].length, 64u * 1024u);
  UT_ASSERT_EQ(ld.memory_regions[0].current, ld.memory_regions[0].origin);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_memory_multiple_regions_and_find)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "MEMORY {\n"
      "  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K\n"
      "  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K\n"
      "}\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_memory_regions, 2);

  int flash_idx = ld_script_find_memory_region(&ld, "FLASH");
  int ram_idx = ld_script_find_memory_region(&ld, "RAM");
  int missing_idx = ld_script_find_memory_region(&ld, "NOPE");

  UT_ASSERT_EQ(flash_idx, 0);
  UT_ASSERT_EQ(ram_idx, 1);
  UT_ASSERT_EQ(missing_idx, -1);
  UT_ASSERT_EQ(ld.memory_regions[ram_idx].attributes, LD_MEM_READ | LD_MEM_WRITE | LD_MEM_EXEC);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_memory_length_suffixes_k_m_g)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "MEMORY {\n"
      "  A (r) : ORIGIN = 0, LENGTH = 2K\n"
      "  B (r) : ORIGIN = 0, LENGTH = 3M\n"
      "  C (r) : ORIGIN = 0, LENGTH = 1G\n"
      "}\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_memory_regions, 3);
  UT_ASSERT_EQ(ld.memory_regions[0].length, 2u * 1024u);
  UT_ASSERT_EQ(ld.memory_regions[1].length, 3u * 1024u * 1024u);
  UT_ASSERT_EQ(ld.memory_regions[2].length, 1u * 1024u * 1024u * 1024u);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_memory_alt_keyword_forms)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  /* org/o and len/l are accepted as abbreviations for ORIGIN/LENGTH. */
  int ret = ld_script_parse_string(&s1, &ld,
      "MEMORY {\n"
      "  FLASH (rx) : org = 0x1000, len = 2K\n"
      "  RAM   (rw) : o = 0x2000, l = 4K\n"
      "}\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_memory_regions, 2);
  UT_ASSERT_EQ(ld.memory_regions[0].origin, 0x1000u);
  UT_ASSERT_EQ(ld.memory_regions[0].length, 2u * 1024u);
  UT_ASSERT_EQ(ld.memory_regions[1].origin, 0x2000u);
  UT_ASSERT_EQ(ld.memory_regions[1].length, 4u * 1024u);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_memory_too_many_regions_reports_error)
{
  TCCState s1;
  LDScript ld;
  char script[4096];
  char *p = script;

  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  p += sprintf(p, "MEMORY {\n");
  for (int i = 0; i < 17; i++)
    p += sprintf(p, "  R%d (r) : ORIGIN = 0x%x, LENGTH = 1K\n", i, i * 0x1000);
  sprintf(p, "}\n");

  int ret = ld_script_parse_string(&s1, &ld, script);

  UT_ASSERT(ret != 0);
  UT_ASSERT_EQ(ld.nb_memory_regions, LD_MAX_MEMORY_REGIONS);

  ld_script_cleanup(&ld);
  return 0;
}

/* BUG C regression pin -- see file header. */
UT_TEST(test_bug_memory_invert_attribute_causes_phantom_regions)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "MEMORY { FLASH (!rx) : ORIGIN = 0x0, LENGTH = 1K }\n");

  /* Currently reports success (0) despite the attribute string never having
   * been parsed correctly and the MEMORY table being corrupted below. A
   * correct implementation should either support '!' or report an error;
   * it should not silently fabricate three extra bogus regions. */
  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_memory_regions, 4);
  UT_ASSERT_STREQ(ld.memory_regions[0].name, "FLASH");
  UT_ASSERT_EQ(ld.memory_regions[0].attributes, 0);
  UT_ASSERT_EQ(ld.memory_regions[0].origin, 0u);
  UT_ASSERT_STREQ(ld.memory_regions[1].name, "rx");
  UT_ASSERT_STREQ(ld.memory_regions[2].name, "ORIGIN");
  UT_ASSERT_STREQ(ld.memory_regions[3].name, "LENGTH");

  ld_script_cleanup(&ld);
  return 0;
}

/* ------------------------------------------------------------------ */
/* PHDRS {}                                                             */
/* ------------------------------------------------------------------ */

UT_TEST(test_phdrs_basic_types)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "PHDRS {\n"
      "  seg_text PT_LOAD;\n"
      "  seg_dyn PT_DYNAMIC;\n"
      "  seg_note PT_NOTE;\n"
      "}\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_phdrs, 3);
  UT_ASSERT_STREQ(ld.phdrs[0].name, "seg_text");
  UT_ASSERT_EQ(ld.phdrs[0].type, PT_LOAD);
  UT_ASSERT_STREQ(ld.phdrs[1].name, "seg_dyn");
  UT_ASSERT_EQ(ld.phdrs[1].type, PT_DYNAMIC);
  UT_ASSERT_STREQ(ld.phdrs[2].name, "seg_note");
  UT_ASSERT_EQ(ld.phdrs[2].type, PT_NOTE);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_phdrs_unknown_type_leaves_type_field_untouched)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);
  /* Poison the slot before parsing: an unrecognized PT_* keyword's
   * strcmp chain has no "else" branch, so ph->type is simply never
   * assigned for it (as opposed to being reset to some sentinel). */
  ld.phdrs[0].type = 0xDEADBEEF;

  int ret = ld_script_parse_string(&s1, &ld, "PHDRS { seg1 PT_TOTALLY_MADE_UP; }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_phdrs, 1);
  UT_ASSERT_STREQ(ld.phdrs[0].name, "seg1");
  UT_ASSERT_EQ(ld.phdrs[0].type, 0xDEADBEEFu);

  ld_script_cleanup(&ld);
  return 0;
}

/* ------------------------------------------------------------------ */
/* ENTRY()                                                              */
/* ------------------------------------------------------------------ */

UT_TEST(test_entry_basic)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld, "ENTRY(_start)\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.has_entry, 1);
  UT_ASSERT_STREQ(ld.entry_point, "_start");

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_entry_name_too_long_reports_error)
{
  TCCState s1;
  LDScript ld;
  char script[300];
  char long_name[200];

  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  memset(long_name, 'a', sizeof(long_name) - 1);
  long_name[sizeof(long_name) - 1] = '\0';
  snprintf(script, sizeof(script), "ENTRY(%s)\n", long_name);

  int ret = ld_script_parse_string(&s1, &ld, script);

  UT_ASSERT(ret != 0);
  UT_ASSERT_EQ(ld.has_entry, 0);

  ld_script_cleanup(&ld);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Expression precedence chain (via SECTIONS {} symbol assignments)     */
/* ------------------------------------------------------------------ */

static addr_t ut_ld_sym_value(LDScript *ld, const char *name)
{
  int idx = ld_script_find_or_create_symbol(ld, name);
  if (idx < 0)
    return (addr_t)-1;
  return ld->symbols[idx].value;
}

UT_TEST(test_expr_add_sub_precedence)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  /* '+'/'-' are same-precedence, left-associative in ld_parse_add(); parens
   * override that grouping. (Multiplication is deliberately NOT exercised
   * here -- see BUG A-mul / test_bug_expr_multiplication_operator_never_applies.) */
  int ret = ld_script_parse_string(&s1, &ld,
      "SECTIONS { a = 10 - 2 - 3; b = 10 - (2 - 3); }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "a"), 5);  /* (10-2)-3, not 10-(2-3) */
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "b"), 11); /* 10-(2-3) == 10-(-1) */

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_expr_shift_and_bitwise_precedence)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "SECTIONS { a = 1 << 2 | 1; b = 0xff & 0x0f; c = 0x0f ^ 0x03; }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "a"), 5);   /* (1<<2)|1, shift binds tighter than or */
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "b"), 0x0f);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "c"), 0x0c);

  ld_script_cleanup(&ld);
  return 0;
}

/* BUG A-mul regression pin (same root cause as BUG A -- see file header,
 * which documents this alongside the '.' phantom-symbol case): '*' is
 * listed in ld_next_token()'s identifier-start character class, so a
 * standalone '*' operator (surrounded by whitespace, as in ordinary
 * arithmetic) is ALWAYS lexed as an LDTOK_NAME token with tok_buf=="*",
 * never as the raw punctuation value '*' (42). ld_parse_mul()'s
 * `while (p->tok == '*' || ...)` therefore never fires for a standalone
 * '*': multiplication silently never applies, `ld_parse_mul()` returns
 * just its left operand, and the cursor is left sitting on the
 * unconsumed "*" token. That leftover token then gets picked up one
 * level further out as if it started a brand-new top-level SECTIONS
 * item: ld_parse_sections' bare-LDTOK_NAME branch treats it as a
 * (bogus) output-section name "*", and the number that followed the
 * '*' in the original expression ("3" in "2 * 3") gets consumed as
 * that bogus section's address. All of this happens silently, with
 * ld_script_parse_string() still reporting success (0). */
UT_TEST(test_bug_expr_multiplication_operator_never_applies)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld, "SECTIONS { a = 2 * 3; }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "a"), 2); /* not 2*3 == 6 */
  /* Side effect: the leftover "*" token got misparsed as a bogus output
   * section, and "3" as its address. */
  UT_ASSERT_EQ(ld.nb_output_sections, 1);
  UT_ASSERT_STREQ(ld.output_sections[0].name, "*");
  UT_ASSERT_EQ(ld.output_sections[0].has_address, 1);
  UT_ASSERT_EQ(ld.output_sections[0].address, 3u);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_expr_hex_and_octal_literals)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  /* "010" is parsed via strtoull(..., base 0) => octal => 8. */
  int ret = ld_script_parse_string(&s1, &ld, "SECTIONS { a = 0x10 + 010; }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "a"), 16 + 8);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_expr_unary_minus_and_bitwise_not)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld, "SECTIONS { a = ~5; b = -3; }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ((uint32_t)ut_ld_sym_value(&ld, "a"), (uint32_t)~5u);
  UT_ASSERT_EQ((uint32_t)ut_ld_sym_value(&ld, "b"), (uint32_t)-3);

  ld_script_cleanup(&ld);
  return 0;
}

/* Division/modulo by zero are silently ignored (the '/' / '%' branches in
 * ld_parse_mul are guarded by `&& val2`), leaving the left-hand value
 * unchanged rather than erroring or crashing. Pinning the current, silent
 * no-op behavior. */
UT_TEST(test_expr_div_mod_and_div_by_zero_is_noop)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "SECTIONS { a = 10 / 3; b = 10 % 3; c = 10 / 0; d = 10 % 0; }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "a"), 3);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "b"), 1);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "c"), 10);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "d"), 10);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_expr_align_builtin_uses_location_counter)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);
  /* Poke location_counter directly (public LDScript field) since the
   * script-level ". = expr;" assignment doesn't reach it -- see BUG A. */
  ld.location_counter = 0x1001;

  int ret = ld_script_parse_string(&s1, &ld, "SECTIONS { a = ALIGN(4); }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "a"), 0x1004u); /* (0x1001+3) & ~3 */

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_expr_origin_and_length_builtins)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K }\n"
      "SECTIONS { a = ORIGIN(FLASH); b = LENGTH(FLASH); }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "a"), 0x08000000u);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "b"), 64u * 1024u);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_expr_defined_builtin)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "SECTIONS { foo = 1; bar = DEFINED(foo); baz = DEFINED(nope); }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "bar"), 1);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "baz"), 0);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_expr_loadaddr_builtin_sets_flag)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "SECTIONS { .text : { } foo = LOADADDR(.text); }\n");

  UT_ASSERT_EQ(ret, 0);
  int text_idx = ld_script_find_output_section(&ld, ".text");
  UT_ASSERT(text_idx >= 0);

  int foo_idx = ld_script_find_or_create_symbol(&ld, "foo");
  UT_ASSERT(foo_idx >= 0);
  UT_ASSERT_EQ(ld.symbols[foo_idx].has_loadaddr, 1);
  UT_ASSERT_EQ(ld.symbols[foo_idx].loadaddr_section_idx, text_idx);
  /* LOADADDR is documented (comment in ld_parse_primary) to evaluate to 0
   * until layout has run -- only the has_loadaddr side-channel carries the
   * real information at parse time. */
  UT_ASSERT_EQ(ld.symbols[foo_idx].value, 0);

  ld_script_cleanup(&ld);
  return 0;
}

/* ------------------------------------------------------------------ */
/* SECTIONS {}                                                          */
/* ------------------------------------------------------------------ */

/* Also the BUG B regression pin -- see file header. */
UT_TEST(test_sections_output_section_dotted_with_patterns_and_keep)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K }\n"
      "SECTIONS { .text : { *(.text*) KEEP(*(.init)) } > FLASH }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_output_sections, 1);
  LDOutputSection *os = &ld.output_sections[0];
  UT_ASSERT_STREQ(os->name, ".text");
  UT_ASSERT_EQ(os->memory_region_idx, 0);

  /* BUG B: every "*(...)" group -- with or without KEEP() -- leaves one
   * extra bogus pattern[] entry with an empty pattern string ahead of the
   * real name(s) parsed from inside the parens. Real content: ".text*"
   * (plain, not kept) and ".init" (kept). Actual: 4 entries, 2 of them
   * blank placeholders. */
  UT_ASSERT_EQ(os->nb_patterns, 4);
  UT_ASSERT_STREQ(os->patterns[0].pattern, "");
  UT_ASSERT_EQ(os->patterns[0].type, LD_PAT_GLOB);
  UT_ASSERT_EQ(os->patterns[0].keep, 0);
  UT_ASSERT_STREQ(os->patterns[1].pattern, ".text*");
  UT_ASSERT_EQ(os->patterns[1].type, LD_PAT_GLOB);
  UT_ASSERT_EQ(os->patterns[1].keep, 0);
  UT_ASSERT_STREQ(os->patterns[2].pattern, "");
  UT_ASSERT_EQ(os->patterns[2].type, LD_PAT_GLOB);
  UT_ASSERT_EQ(os->patterns[2].keep, 1);
  UT_ASSERT_STREQ(os->patterns[3].pattern, ".init");
  UT_ASSERT_EQ(os->patterns[3].type, LD_PAT_EXACT);
  UT_ASSERT_EQ(os->patterns[3].keep, 1);

  /* The real-world-relevant surface (should_keep()) is unaffected: the
   * bogus empty pattern can never match a real (non-empty) section name. */
  UT_ASSERT_EQ(ld_section_should_keep(&ld, ".init"), 1);
  UT_ASSERT_EQ(ld_section_should_keep(&ld, ".text"), 0); /* matched, but keep==0 */

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_sections_output_section_bare_name_form)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  /* Output-section names without a leading '.' take a separate code path
   * (ld_parse_sections' bare-LDTOK_NAME branch) from the "dotted" one, but
   * -- see BUG A's header comment -- a leading-dot name like ".text" is
   * ALSO lexed as a single LDTOK_NAME token (the identifier scanner treats
   * '.' as a valid identifier-start character), so in practice *both*
   * forms are handled by this same bare-name branch. */
  int ret = ld_script_parse_string(&s1, &ld, "SECTIONS { my_data : { *(.data) } }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_output_sections, 1);
  UT_ASSERT_STREQ(ld.output_sections[0].name, "my_data");

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_sections_provide_and_provide_hidden)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "SECTIONS { .data : { PROVIDE(sym_a = 0x10); PROVIDE_HIDDEN(sym_b = 0x20); } }\n");

  UT_ASSERT_EQ(ret, 0);
  int a_idx = ld_script_find_or_create_symbol(&ld, "sym_a");
  int b_idx = ld_script_find_or_create_symbol(&ld, "sym_b");

  UT_ASSERT_EQ(ld.symbols[a_idx].value, 0x10);
  UT_ASSERT_EQ(ld.symbols[a_idx].visibility, LD_SYM_PROVIDE);
  UT_ASSERT_EQ(ld.symbols[a_idx].defined, 1);

  UT_ASSERT_EQ(ld.symbols[b_idx].value, 0x20);
  UT_ASSERT_EQ(ld.symbols[b_idx].visibility, LD_SYM_PROVIDE_HIDDEN);
  UT_ASSERT_EQ(ld.symbols[b_idx].defined, 1);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_sections_symbol_assignment_via_expression)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld, "SECTIONS { foo = 0x2000; bar = foo + 4; }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "foo"), 0x2000u);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "bar"), 0x2004u);

  ld_script_cleanup(&ld);
  return 0;
}

/* BUG A regression pin -- see file header. Exercises both the top-level
 * SECTIONS {} form and the nested-inside-an-output-section-body form; both
 * take the same "generic symbol assignment" fallback path. */
UT_TEST(test_bug_location_counter_dot_is_treated_as_phantom_symbol)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld, "SECTIONS { . = 0x1000; foo = .; }\n");

  UT_ASSERT_EQ(ret, 0);
  /* The real location counter never moves... */
  UT_ASSERT_EQ(ld.location_counter, 0);
  /* ...because ". = 0x1000;" instead created/updated a symbol literally
   * named ".", and "foo = .;" read that phantom symbol's value back. */
  int dot_idx = ld_script_find_or_create_symbol(&ld, ".");
  UT_ASSERT(dot_idx >= 0);
  UT_ASSERT_EQ(ld.symbols[dot_idx].value, 0x1000u);
  UT_ASSERT_EQ(ld.symbols[dot_idx].defined, 1);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "foo"), 0x1000u);

  ld_script_cleanup(&ld);

  /* Same phantom-symbol mechanism inside a nested output-section body. */
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);
  ret = ld_script_parse_string(&s1, &ld, "SECTIONS { .data : { . = 0x2000; bar = .; } }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.location_counter, 0);
  UT_ASSERT_EQ(ut_ld_sym_value(&ld, "bar"), 0x2000u);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_sections_region_at_and_phdr_supported_order)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  /* This parser only recognizes "> REGION :PHDR AT > LMA_REGION" (phdr tag
   * *before* AT) -- see BUG D. */
  int ret = ld_script_parse_string(&s1, &ld,
      "MEMORY { FLASH (rx) : ORIGIN = 0x0, LENGTH = 1K  RAM (rwx) : ORIGIN = 0x1000, LENGTH = 1K }\n"
      "PHDRS { text_seg PT_LOAD; }\n"
      "SECTIONS { .data : { *(.data) } > RAM :text_seg AT > FLASH }\n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_output_sections, 1);
  LDOutputSection *os = &ld.output_sections[0];
  UT_ASSERT_EQ(os->memory_region_idx, 1);      /* RAM */
  UT_ASSERT_EQ(os->load_memory_region_idx, 0); /* FLASH */
  UT_ASSERT_EQ(os->phdr_idx, 0);               /* text_seg */

  ld_script_cleanup(&ld);
  return 0;
}

/* BUG D regression pin -- see file header. */
UT_TEST(test_bug_sections_standard_region_at_phdr_order_drops_phdr)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  /* GNU ld's conventional field order: AT before the phdr tag. */
  int ret = ld_script_parse_string(&s1, &ld,
      "MEMORY { FLASH (rx) : ORIGIN = 0x0, LENGTH = 1K  RAM (rwx) : ORIGIN = 0x1000, LENGTH = 1K }\n"
      "PHDRS { text_seg PT_LOAD; }\n"
      "SECTIONS { .data : { *(.data) } > RAM AT > FLASH :text_seg }\n");

  UT_ASSERT_EQ(ret, 0); /* no error reported */
  UT_ASSERT_EQ(ld.nb_output_sections, 1);
  LDOutputSection *os = &ld.output_sections[0];
  UT_ASSERT_EQ(os->memory_region_idx, 1);
  UT_ASSERT_EQ(os->load_memory_region_idx, 0);
  UT_ASSERT_EQ(os->phdr_idx, -1); /* silently dropped */

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_find_output_section_found_and_not_found)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld, "SECTIONS { .text : { } }\n");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(ld_script_find_output_section(&ld, ".text"), 0);
  UT_ASSERT_EQ(ld_script_find_output_section(&ld, ".missing"), -1);

  ld_script_cleanup(&ld);
  return 0;
}

/* ------------------------------------------------------------------ */
/* ld_section_matches_pattern()                                         */
/* ------------------------------------------------------------------ */

UT_TEST(test_pattern_exact_and_question_mark_match)
{
  UT_ASSERT_EQ(ld_section_matches_pattern(".text", ".text"), 1);
  UT_ASSERT_EQ(ld_section_matches_pattern(".data", ".text"), 0);
  UT_ASSERT_EQ(ld_section_matches_pattern(".text", ".t?xt"), 1);
  UT_ASSERT_EQ(ld_section_matches_pattern(".txt", ".t?xt"), 0); /* '?' needs exactly one char */
  return 0;
}

UT_TEST(test_pattern_trailing_wildcard_matches_suffix)
{
  UT_ASSERT_EQ(ld_section_matches_pattern(".text", ".text*"), 1);
  UT_ASSERT_EQ(ld_section_matches_pattern(".text.hot", ".text*"), 1);
  UT_ASSERT_EQ(ld_section_matches_pattern(".data", ".text*"), 0);
  return 0;
}

UT_TEST(test_pattern_mid_string_wildcard_backtracks)
{
  UT_ASSERT_EQ(ld_section_matches_pattern(".text.hot.o", ".text.*.o"), 1);
  UT_ASSERT_EQ(ld_section_matches_pattern(".text.hot.c", ".text.*.o"), 0);
  return 0;
}

UT_TEST(test_pattern_star_alone_matches_everything_including_empty)
{
  UT_ASSERT_EQ(ld_section_matches_pattern(".bss", "*"), 1);
  UT_ASSERT_EQ(ld_section_matches_pattern("", "*"), 1);
  return 0;
}

/* ------------------------------------------------------------------ */
/* ld_section_should_keep()                                             */
/* ------------------------------------------------------------------ */

UT_TEST(test_should_keep_matches_keep_pattern_and_null_safety)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "SECTIONS { .isr : { KEEP(*(.isr_vector)) } }\n");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(ld_section_should_keep(&ld, ".isr_vector"), 1);
  UT_ASSERT_EQ(ld_section_should_keep(&ld, ".unrelated"), 0);
  UT_ASSERT_EQ(ld_section_should_keep(NULL, ".isr_vector"), 0);

  ld_script_cleanup(&ld);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Symbol table helpers                                                 */
/* ------------------------------------------------------------------ */

UT_TEST(test_find_or_create_symbol_creates_and_reuses)
{
  LDScript ld;
  ld_script_init(&ld);

  int idx1 = ld_script_find_or_create_symbol(&ld, "abc");
  UT_ASSERT(idx1 >= 0);
  UT_ASSERT_EQ(ld.symbols[idx1].defined, 0);
  UT_ASSERT_STREQ(ld.symbols[idx1].name, "abc");

  int idx1_again = ld_script_find_or_create_symbol(&ld, "abc");
  UT_ASSERT_EQ(idx1_again, idx1);
  UT_ASSERT_EQ(ld.nb_symbols, 1);

  int idx2 = ld_script_find_or_create_symbol(&ld, "xyz");
  UT_ASSERT(idx2 != idx1);
  UT_ASSERT_EQ(ld.nb_symbols, 2);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_add_standard_symbols_registers_all_and_are_findable)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_add_standard_symbols(&s1, &ld);

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_symbols, 20);

  int end_idx = ld_script_find_or_create_symbol(&ld, "_end");
  UT_ASSERT(end_idx >= 0);
  UT_ASSERT_EQ(ld.nb_symbols, 20); /* already existed, not re-created */
  UT_ASSERT_EQ(ld.symbols[end_idx].defined, 0);

  ld_script_cleanup(&ld);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Malformed / edge input                                               */
/* ------------------------------------------------------------------ */

UT_TEST(test_parse_empty_script_succeeds_trivially)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld, "");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_memory_regions, 0);
  UT_ASSERT_EQ(ld.nb_output_sections, 0);
  UT_ASSERT_EQ(ld.nb_symbols, 0);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_parse_whitespace_and_comments_only_succeeds)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "  \t\n /* a block comment\n spanning lines */ \n // a line comment\n   \n");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_memory_regions, 0);
  UT_ASSERT_EQ(ld.nb_output_sections, 0);
  UT_ASSERT_EQ(ld.nb_symbols, 0);

  ld_script_cleanup(&ld);
  return 0;
}

UT_TEST(test_parse_unterminated_memory_block_reports_error)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "MEMORY { FLASH (rx) : ORIGIN = 0x1000, LENGTH = 1K");

  UT_ASSERT(ret != 0);

  ld_script_cleanup(&ld);
  return 0;
}

/* Stray top-level punctuation that never matches the LDTOK_NAME dispatch
 * (MEMORY/PHDRS/SECTIONS/ENTRY) is silently skipped, one token at a time,
 * rather than being reported as a syntax error -- consistent with the same
 * permissive "unrecognized token -> skip and continue" pattern documented
 * for BUG C. Not crashing, and not corrupting any state here (there's
 * nothing for stray ';' tokens to be misinterpreted as), so this is
 * pinned as a documented laxity rather than filed as its own bug. */
UT_TEST(test_parse_unknown_top_level_token_is_silently_skipped)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld, " ; ; ; ");

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(ld.nb_memory_regions, 0);
  UT_ASSERT_EQ(ld.nb_output_sections, 0);
  UT_ASSERT_EQ(ld.nb_symbols, 0);

  ld_script_cleanup(&ld);
  return 0;
}

/* ------------------------------------------------------------------ */
/* ld_script_dump()                                                     */
/* ------------------------------------------------------------------ */

UT_TEST(test_dump_smoke_does_not_crash)
{
  TCCState s1;
  LDScript ld;
  memset(&s1, 0, sizeof(s1));
  ld_script_init(&ld);

  int ret = ld_script_parse_string(&s1, &ld,
      "ENTRY(_start)\n"
      "MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K }\n"
      "PHDRS { text_seg PT_LOAD; }\n"
      "SECTIONS { .text : { *(.text*) KEEP(*(.init)) } > FLASH :text_seg }\n");
  UT_ASSERT_EQ(ret, 0);

  ut_ld_script_dump_quiet(&ld);

  ld_script_cleanup(&ld);
  return 0;
}

UT_SUITE(ld_script)
{
  UT_RUN(test_memory_single_region_basic);
  UT_RUN(test_memory_multiple_regions_and_find);
  UT_RUN(test_memory_length_suffixes_k_m_g);
  UT_RUN(test_memory_alt_keyword_forms);
  UT_RUN(test_memory_too_many_regions_reports_error);
  UT_RUN(test_bug_memory_invert_attribute_causes_phantom_regions);

  UT_RUN(test_phdrs_basic_types);
  UT_RUN(test_phdrs_unknown_type_leaves_type_field_untouched);

  UT_RUN(test_entry_basic);
  UT_RUN(test_entry_name_too_long_reports_error);

  UT_RUN(test_expr_add_sub_precedence);
  UT_RUN(test_expr_shift_and_bitwise_precedence);
  UT_RUN(test_bug_expr_multiplication_operator_never_applies);
  UT_RUN(test_expr_hex_and_octal_literals);
  UT_RUN(test_expr_unary_minus_and_bitwise_not);
  UT_RUN(test_expr_div_mod_and_div_by_zero_is_noop);
  UT_RUN(test_expr_align_builtin_uses_location_counter);
  UT_RUN(test_expr_origin_and_length_builtins);
  UT_RUN(test_expr_defined_builtin);
  UT_RUN(test_expr_loadaddr_builtin_sets_flag);

  UT_RUN(test_sections_output_section_dotted_with_patterns_and_keep);
  UT_RUN(test_sections_output_section_bare_name_form);
  UT_RUN(test_sections_provide_and_provide_hidden);
  UT_RUN(test_sections_symbol_assignment_via_expression);
  UT_RUN(test_bug_location_counter_dot_is_treated_as_phantom_symbol);
  UT_RUN(test_sections_region_at_and_phdr_supported_order);
  UT_RUN(test_bug_sections_standard_region_at_phdr_order_drops_phdr);
  UT_RUN(test_find_output_section_found_and_not_found);

  UT_RUN(test_pattern_exact_and_question_mark_match);
  UT_RUN(test_pattern_trailing_wildcard_matches_suffix);
  UT_RUN(test_pattern_mid_string_wildcard_backtracks);
  UT_RUN(test_pattern_star_alone_matches_everything_including_empty);

  UT_RUN(test_should_keep_matches_keep_pattern_and_null_safety);

  UT_RUN(test_find_or_create_symbol_creates_and_reuses);
  UT_RUN(test_add_standard_symbols_registers_all_and_are_findable);

  UT_RUN(test_parse_empty_script_succeeds_trivially);
  UT_RUN(test_parse_whitespace_and_comments_only_succeeds);
  UT_RUN(test_parse_unterminated_memory_block_reports_error);
  UT_RUN(test_parse_unknown_top_level_token_is_silently_skipped);

  UT_RUN(test_dump_smoke_does_not_crash);
}
