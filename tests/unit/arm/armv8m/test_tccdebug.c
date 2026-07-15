/*
 *  test_tccdebug.c - suite for tccdebug.c diagnostic printers
 */

#define _POSIX_C_SOURCE 200809L
#define USING_GLOBALS
#include "tccdebug.h"
#include "ut.h"

#include <unistd.h>

void utb_set_tok_str(int tok, const char *name);

struct stderr_capture
{
  FILE *tmp;
  int saved_fd;
  char buf[4096];
};

static int capture_stderr_begin(struct stderr_capture *cap)
{
  memset(cap, 0, sizeof(*cap));
  fflush(stderr);
  cap->saved_fd = dup(fileno(stderr));
  if (cap->saved_fd < 0)
    return -1;
  cap->tmp = tmpfile();
  if (!cap->tmp)
  {
    close(cap->saved_fd);
    cap->saved_fd = -1;
    return -1;
  }
  if (dup2(fileno(cap->tmp), fileno(stderr)) < 0)
  {
    fclose(cap->tmp);
    close(cap->saved_fd);
    cap->tmp = NULL;
    cap->saved_fd = -1;
    return -1;
  }
  return 0;
}

static const char *capture_stderr_end(struct stderr_capture *cap)
{
  size_t n;

  fflush(stderr);
  fseek(cap->tmp, 0, SEEK_SET);
  n = fread(cap->buf, 1, sizeof(cap->buf) - 1, cap->tmp);
  cap->buf[n] = '\0';

  dup2(cap->saved_fd, fileno(stderr));
  close(cap->saved_fd);
  fclose(cap->tmp);
  cap->saved_fd = -1;
  cap->tmp = NULL;
  return cap->buf;
}

static int str_has(const char *s, const char *needle)
{
  return strstr(s, needle) != NULL;
}

UT_TEST(test_svalue_null)
{
  struct stderr_capture cap;
  UT_ASSERT_EQ(capture_stderr_begin(&cap), 0);
  tcc_debug_print_svalue(NULL);
  const char *out = capture_stderr_end(&cap);

  UT_ASSERT_STREQ(out, "SValue(NULL)\n");
  return 0;
}

UT_TEST(test_svalue_const_with_type_modifiers_and_spill)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.r = VT_CONST | VT_LVAL | VT_PARAM | VT_SYM | VT_NONCONST | VT_BOUNDED;
  sv.c.i = -42;
  sv.type.t = VT_UNSIGNED | VT_SHORT | VT_CONSTANT | VT_VOLATILE;
  sv.vr = 7;
  sv.pr0_reg = 3;
  sv.pr0_spilled = 1;
  sv.pr1_reg = 4;

  struct stderr_capture cap;
  UT_ASSERT_EQ(capture_stderr_begin(&cap), 0);
  tcc_debug_print_svalue(&sv);
  const char *out = capture_stderr_end(&cap);

  UT_ASSERT(str_has(out, "SValue{ loc=CONST(0x10)"));
  UT_ASSERT(str_has(out, "mods=LVAL|PARAM|SYM|NONCONST|BOUNDED"));
  UT_ASSERT(str_has(out, ", c=-42"));
  UT_ASSERT(str_has(out, ", type=unsigned short const volatile"));
  UT_ASSERT(str_has(out, ", vr=7, pr0=35, pr1=4"));
  UT_ASSERT(str_has(out, " }\n"));
  return 0;
}

UT_TEST(test_svalue_local_and_array_vla_bitfield)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.r = VT_LLOCAL | VT_MUSTCAST | VT_MUSTBOUND;
  sv.c.i = -128;
  sv.type.t = VT_PTR | VT_ARRAY | VT_VLA | VT_BITFIELD;
  sv.vr = -1;
  sv.pr0_reg = PREG_REG_NONE;
  sv.pr1_reg = PREG_REG_NONE;

  struct stderr_capture cap;
  UT_ASSERT_EQ(capture_stderr_begin(&cap), 0);
  tcc_debug_print_svalue(&sv);
  const char *out = capture_stderr_end(&cap);

  UT_ASSERT(str_has(out, "loc=LLOCAL(0x11)"));
  UT_ASSERT(str_has(out, "mods=MUSTCAST|MUSTBOUND"));
  UT_ASSERT(str_has(out, ", off=-128"));
  UT_ASSERT(str_has(out, ", type=ptr*[](vla)(bitfield)"));
  UT_ASSERT(str_has(out, ", vr=-1, pr0=31, pr1=31"));
  return 0;
}

UT_TEST(test_svalue_register_location_has_no_modifiers)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.r = 2;
  sv.type.t = VT_INT;
  sv.pr0_reg = PREG_REG_NONE;
  sv.pr1_reg = PREG_REG_NONE;

  struct stderr_capture cap;
  UT_ASSERT_EQ(capture_stderr_begin(&cap), 0);
  tcc_debug_print_svalue(&sv);
  const char *out = capture_stderr_end(&cap);

  UT_ASSERT(str_has(out, "loc=REG(2), mods=-"));
  UT_ASSERT(str_has(out, ", type=int"));
  return 0;
}

UT_TEST(test_sym_null)
{
  struct stderr_capture cap;
  UT_ASSERT_EQ(capture_stderr_begin(&cap), 0);
  tcc_debug_print_sym(NULL);
  const char *out = capture_stderr_end(&cap);

  UT_ASSERT_STREQ(out, "Sym(NULL)\n");
  return 0;
}

UT_TEST(test_sym_prints_token_r_type_and_attrs)
{
  enum
  {
    TOK_DEBUG_SYM = 733
  };
  Sym s;
  memset(&s, 0, sizeof(s));
  s.v = TOK_DEBUG_SYM;
  s.r = VT_LOCAL | VT_LVAL | VT_SYM;
  s.vreg = 12;
  s.type.t = VT_UNSIGNED | VT_SHORT | VT_ARRAY | VT_CONSTANT;
  s.a.aligned = 4;
  s.a.packed = 1;
  s.a.weak = 1;
  s.a.visibility = 2;
  s.a.dllimport = 1;
  s.a.nodecorate = 1;
  s.a.addrtaken = 1;
  s.a.nodebug = 1;
  s.a.naked = 1;
  utb_set_tok_str(TOK_DEBUG_SYM, "debug_symbol");

  struct stderr_capture cap;
  UT_ASSERT_EQ(capture_stderr_begin(&cap), 0);
  tcc_debug_print_sym(&s);
  const char *out = capture_stderr_end(&cap);
  utb_set_tok_str(TOK_DEBUG_SYM, NULL);

  UT_ASSERT(str_has(out, "Sym{ v=733('debug_symbol')"));
  UT_ASSERT(str_has(out, "r={loc=LOCAL(0x12), mods=LVAL|SYM} (0x00d2)"));
  UT_ASSERT(str_has(out, ", vreg=12"));
  UT_ASSERT(str_has(out, ", type=unsigned short[] const"));
  UT_ASSERT(str_has(out, ", attr=aligned=4|packed|weak|vis=2|dllimport|nodecorate|addrtaken|nodebug|naked"));
  UT_ASSERT(str_has(out, ", next="));
  UT_ASSERT(str_has(out, ", prev="));
  UT_ASSERT(str_has(out, ", prev_tok="));
  return 0;
}

UT_TEST(test_sym_defaults_unknown_token_and_empty_attrs)
{
  Sym s;
  memset(&s, 0, sizeof(s));
  s.v = 999;
  s.r = VT_CONST;
  s.type.t = VT_FUNC;

  struct stderr_capture cap;
  UT_ASSERT_EQ(capture_stderr_begin(&cap), 0);
  tcc_debug_print_sym(&s);
  const char *out = capture_stderr_end(&cap);

  UT_ASSERT(str_has(out, "Sym{ v=999('?')"));
  UT_ASSERT(str_has(out, "r={loc=CONST(0x10), mods=-}"));
  UT_ASSERT(str_has(out, ", type=func"));
  UT_ASSERT(str_has(out, ", attr=-"));
  return 0;
}
