/*
 *  test_tcc.c - white-box unit tests for isolated helpers in tcc.c
 *  (build_tcc/run_unit_tests_tcc)
 *
 *  Tests the driver helpers and the early-exit paths of main().  The source
 *  file is pulled in directly so that static/ST_FUNC helpers are visible.
 */

#define USING_GLOBALS
#include "tcc.h"
#include "ut.h"

#include <fcntl.h>
#include <unistd.h>

/* Forward declare TCCState so the stub prototypes below match libtcc.h. */
typedef struct TCCState TCCState;

/* Stubs for production helpers referenced by tcc.c but not linked into this
   unit-test binary.  Definitions follow after tcc.c is included. */
extern int tcc_add_sysinclude_path(TCCState *s, const char *path);
extern int tcc_add_include_path(TCCState *s, const char *path);
extern int tcc_add_library_path(TCCState *s, const char *path);
extern const char *default_elfinterp(TCCState *s);

/* Stubs needed to exercise the early-exit paths of main(). */
extern void tcc_set_realloc(void *(*my_realloc)(void *, unsigned long));
extern TCCState *tcc_new(void);
extern int tcc_parse_args(TCCState *s, int *argc, char ***argv, int optind);
extern void tcc_delete(TCCState *s);

/* Stubs for the production calls made by main() so the whole function links. */
extern int tcc_set_output_type(TCCState *s, int output_type);
extern int tcc_add_library(TCCState *s, const char *libraryname);
extern int tcc_add_file(TCCState *s, const char *filename);
extern int tcc_group_has_satisfiable_undefs(TCCState *s1);
extern int tcc_output_file(TCCState *s, const char *filename);

/* Rename main() so the test harness keeps its own entry point. */
#define main tcc_ut_main
#include "tcc.c"
#undef main

/* ========================================================================
 * stdout capture helper
 * ======================================================================== */

struct captured_stdout
{
  char *data;
  size_t len;
};

static int capture_stdout(struct captured_stdout *out, void (*fn)(void *),
                          void *arg)
{
  char path[] = "/tmp/tcc_ut_stdoutXXXXXX";
  int tmp_fd = mkstemp(path);
  int saved_stdout;
  off_t len;

  if (tmp_fd < 0)
    return -1;
  unlink(path);

  saved_stdout = dup(STDOUT_FILENO);
  if (saved_stdout < 0)
  {
    close(tmp_fd);
    return -1;
  }

  if (dup2(tmp_fd, STDOUT_FILENO) < 0)
  {
    close(saved_stdout);
    close(tmp_fd);
    return -1;
  }

  fn(arg);
  fflush(stdout);

  if (dup2(saved_stdout, STDOUT_FILENO) < 0)
  {
    close(saved_stdout);
    close(tmp_fd);
    return -1;
  }
  close(saved_stdout);

  len = lseek(tmp_fd, 0, SEEK_END);
  if (len < 0 || lseek(tmp_fd, 0, SEEK_SET) < 0)
  {
    close(tmp_fd);
    return -1;
  }

  out->data = (char *)tcc_malloc(len + 1);
  if (!out->data)
  {
    close(tmp_fd);
    return -1;
  }

  out->len = (size_t)read(tmp_fd, out->data, len);
  close(tmp_fd);
  out->data[out->len] = '\0';
  return 0;
}

static void free_captured_stdout(struct captured_stdout *out)
{
  tcc_free(out->data);
  out->data = NULL;
  out->len = 0;
}

/* ========================================================================
 * stubs for tcc.c helpers not supplied by tcc_stubs.c
 * ======================================================================== */

static const char *captured_sysinclude_path = NULL;
static const char *captured_include_path = NULL;
static const char *captured_library_path = NULL;

int tcc_add_sysinclude_path(TCCState *s, const char *path)
{
  (void)s;
  captured_sysinclude_path = path;
  return 0;
}

int tcc_add_include_path(TCCState *s, const char *path)
{
  (void)s;
  captured_include_path = path;
  return 0;
}

int tcc_add_library_path(TCCState *s, const char *path)
{
  (void)s;
  captured_library_path = path;
  return 0;
}

const char *default_elfinterp(TCCState *s)
{
  (void)s;
  return "/lib/ld-linux-armhf.so.3";
}

static int main_stub_parse_args_ret = 0;
static int main_stub_allocated_state_verbose = 0;
static int main_stub_allocated_state_setup = 0;
static int main_stub_allocated_state_nb_files = 0;
static int main_stub_allocated_state_nb_libraries = 0;
static int main_stub_allocated_state_nb_errors = 0;
static struct filespec *main_stub_allocated_state_filespec = NULL;
static int main_stub_allocated_state_output_type = 0;

void tcc_set_realloc(void *(*my_realloc)(void *, unsigned long))
{
  (void)my_realloc;
}

TCCState *tcc_new(void)
{
  TCCState *s = (TCCState *)tcc_mallocz(sizeof(TCCState));
  s->verbose = main_stub_allocated_state_verbose;
  if (main_stub_allocated_state_setup)
  {
    s->nb_files = main_stub_allocated_state_nb_files;
    s->nb_libraries = main_stub_allocated_state_nb_libraries;
    s->nb_errors = main_stub_allocated_state_nb_errors;
    s->files = &main_stub_allocated_state_filespec;
    s->output_type = main_stub_allocated_state_output_type;
  }
  return s;
}

int tcc_parse_args(TCCState *s, int *argc, char ***argv, int optind)
{
  (void)s;
  (void)argc;
  (void)argv;
  (void)optind;
  return main_stub_parse_args_ret;
}

void tcc_delete(TCCState *s)
{
  tcc_free(s);
}

int tcc_set_output_type(TCCState *s, int output_type)
{
  (void)s;
  (void)output_type;
  return 0;
}

int tcc_add_library(TCCState *s, const char *libraryname)
{
  (void)s;
  (void)libraryname;
  return 0;
}

int tcc_add_file(TCCState *s, const char *filename)
{
  (void)s;
  (void)filename;
  return 0;
}

int tcc_group_has_satisfiable_undefs(TCCState *s1)
{
  (void)s1;
  return 0;
}

int tcc_output_file(TCCState *s, const char *filename)
{
  (void)s;
  (void)filename;
  return 0;
}

/* ========================================================================
 * print_dirs
 * ======================================================================== */

struct print_dirs_args
{
  const char *msg;
  char **paths;
  int nb_paths;
};

static void call_print_dirs(void *arg)
{
  struct print_dirs_args *a = arg;
  print_dirs(a->msg, a->paths, a->nb_paths);
}

UT_TEST(test_print_dirs_shows_dash_for_empty_list)
{
  struct captured_stdout cap = {0};
  struct print_dirs_args args = {"include", NULL, 0};

  UT_ASSERT_EQ(capture_stdout(&cap, call_print_dirs, &args), 0);
  UT_ASSERT_STREQ(cap.data, "include:\n  -\n");
  free_captured_stdout(&cap);
  return 0;
}

UT_TEST(test_print_dirs_lists_each_path)
{
  struct captured_stdout cap = {0};
  char p1[] = "/usr/include";
  char p2[] = "/usr/local/include";
  char *paths[] = {p1, p2};
  struct print_dirs_args args = {"libraries", paths, 2};

  UT_ASSERT_EQ(capture_stdout(&cap, call_print_dirs, &args), 0);
  UT_ASSERT_STREQ(cap.data,
                  "libraries:\n  /usr/include\n  /usr/local/include\n");
  free_captured_stdout(&cap);
  return 0;
}

/* ========================================================================
 * print_search_dirs
 * ======================================================================== */

struct print_search_dirs_args
{
  TCCState *s;
};

static void call_print_search_dirs(void *arg)
{
  struct print_search_dirs_args *a = arg;
  print_search_dirs(a->s);
}

UT_TEST(test_print_search_dirs_outputs_all_sections)
{
  struct captured_stdout cap = {0};
  TCCState s;
  char inc1[] = "/usr/include";
  char inc2[] = "/usr/local/include";
  char *sysincludes[] = {inc1, inc2};
  char lib1[] = "/usr/lib";
  char *libraries[] = {lib1};
  char crt1[] = "/usr/lib/crt1.o";
  char crt2[] = "/usr/lib/crti.o";
  char *crts[] = {crt1, crt2};
  struct print_search_dirs_args args;

  memset(&s, 0, sizeof(s));
  s.tcc_lib_path = "/opt/tcc";
  s.sysinclude_paths = sysincludes;
  s.nb_sysinclude_paths = 2;
  s.library_paths = libraries;
  s.nb_library_paths = 1;
  s.crt_paths = crts;
  s.nb_crt_paths = 2;

  args.s = &s;
  UT_ASSERT_EQ(capture_stdout(&cap, call_print_search_dirs, &args), 0);

  UT_ASSERT(strstr(cap.data, "install: /opt/tcc") != NULL);
  UT_ASSERT(strstr(cap.data,
                   "include:\n  /usr/include\n  /usr/local/include")
            != NULL);
  UT_ASSERT(strstr(cap.data, "libraries:\n  /usr/lib") != NULL);
  UT_ASSERT(strstr(cap.data, "libtcc1:\n  /usr/lib/libtcc1.a") != NULL);
  UT_ASSERT(strstr(cap.data,
                   "crt:\n  /usr/lib/crt1.o\n  /usr/lib/crti.o")
            != NULL);
  UT_ASSERT(strstr(cap.data,
                   "elfinterp:\n  /lib/ld-linux-armhf.so.3")
            != NULL);

  free_captured_stdout(&cap);
  return 0;
}

/* ========================================================================
 * set_environment
 * ======================================================================== */

static void save_env(const char *name, char **out)
{
  const char *v = getenv(name);
  *out = v ? tcc_strdup(v) : NULL;
}

static void restore_env(const char *name, char *saved)
{
  if (saved)
    setenv(name, saved, 1);
  else
    unsetenv(name);
  tcc_free(saved);
}

UT_TEST(test_set_environment_picks_up_c_include_path)
{
  char *saved = NULL;
  TCCState s;

  memset(&s, 0, sizeof(s));
  save_env("C_INCLUDE_PATH", &saved);
  setenv("C_INCLUDE_PATH", "/ci/path", 1);

  captured_sysinclude_path = NULL;
  captured_include_path = NULL;
  captured_library_path = NULL;
  set_environment(&s);

  UT_ASSERT_STREQ(captured_sysinclude_path, "/ci/path");
  UT_ASSERT_EQ(captured_include_path, NULL);
  UT_ASSERT_EQ(captured_library_path, NULL);

  restore_env("C_INCLUDE_PATH", saved);
  return 0;
}

UT_TEST(test_set_environment_picks_up_cpath)
{
  char *saved = NULL;
  TCCState s;

  memset(&s, 0, sizeof(s));
  save_env("CPATH", &saved);
  setenv("CPATH", "/cpath", 1);

  captured_sysinclude_path = NULL;
  captured_include_path = NULL;
  captured_library_path = NULL;
  set_environment(&s);

  UT_ASSERT_EQ(captured_sysinclude_path, NULL);
  UT_ASSERT_STREQ(captured_include_path, "/cpath");
  UT_ASSERT_EQ(captured_library_path, NULL);

  restore_env("CPATH", saved);
  return 0;
}

UT_TEST(test_set_environment_picks_up_library_path)
{
  char *saved = NULL;
  TCCState s;

  memset(&s, 0, sizeof(s));
  save_env("LIBRARY_PATH", &saved);
  setenv("LIBRARY_PATH", "/lib/path", 1);

  captured_sysinclude_path = NULL;
  captured_include_path = NULL;
  captured_library_path = NULL;
  set_environment(&s);

  UT_ASSERT_EQ(captured_sysinclude_path, NULL);
  UT_ASSERT_EQ(captured_include_path, NULL);
  UT_ASSERT_STREQ(captured_library_path, "/lib/path");

  restore_env("LIBRARY_PATH", saved);
  return 0;
}

UT_TEST(test_set_environment_handles_all_three_variables)
{
  char *saved_c = NULL, *saved_p = NULL, *saved_l = NULL;
  TCCState s;

  memset(&s, 0, sizeof(s));
  save_env("C_INCLUDE_PATH", &saved_c);
  save_env("CPATH", &saved_p);
  save_env("LIBRARY_PATH", &saved_l);
  setenv("C_INCLUDE_PATH", "/a", 1);
  setenv("CPATH", "/b", 1);
  setenv("LIBRARY_PATH", "/c", 1);

  captured_sysinclude_path = NULL;
  captured_include_path = NULL;
  captured_library_path = NULL;
  set_environment(&s);

  UT_ASSERT_STREQ(captured_sysinclude_path, "/a");
  UT_ASSERT_STREQ(captured_include_path, "/b");
  UT_ASSERT_STREQ(captured_library_path, "/c");

  restore_env("C_INCLUDE_PATH", saved_c);
  restore_env("CPATH", saved_p);
  restore_env("LIBRARY_PATH", saved_l);
  return 0;
}

/* ========================================================================
 * main() early-exit paths (renamed to tcc_ut_main)
 * ======================================================================== */

struct main_args
{
  int argc;
  char **argv;
};

static int main_stub_last_return = 0;

static void call_tcc_ut_main(void *arg)
{
  struct main_args *a = arg;
  main_stub_last_return = tcc_ut_main(a->argc, a->argv);
}

UT_TEST(test_main_help_returns_zero)
{
  char *argv[] = {"tcc", "-h", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};

  main_stub_parse_args_ret = 1; /* OPT_HELP */
  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT(strstr(cap.data, "Usage:") != NULL);
  UT_ASSERT_EQ(main_stub_last_return, 0);
  free_captured_stdout(&cap);
  return 0;
}

UT_TEST(test_main_version_returns_zero)
{
  char *argv[] = {"tcc", "-v", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};

  main_stub_parse_args_ret = 3; /* OPT_V */
  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_STREQ(cap.data, "");
  UT_ASSERT_EQ(main_stub_last_return, 0);
  free_captured_stdout(&cap);
  return 0;
}

UT_TEST(test_main_parse_failure_returns_one)
{
  char *argv[] = {"tcc", "-bad", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};

  main_stub_parse_args_ret = -1;
  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 1);
  free_captured_stdout(&cap);
  return 0;
}

UT_TEST(test_main_verbose_version_prints_version)
{
  char *argv[] = {"tcc", "-v", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};

  main_stub_parse_args_ret = 3; /* OPT_V */
  main_stub_allocated_state_verbose = 1;
  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT(strstr(cap.data, "tcc version") != NULL);
  UT_ASSERT_EQ(main_stub_last_return, 0);
  free_captured_stdout(&cap);
  main_stub_allocated_state_verbose = 0;
  return 0;
}

UT_TEST(test_main_verbose_help_prints_both_helps)
{
  char *argv[] = {"tcc", "-h", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};

  main_stub_parse_args_ret = 1; /* OPT_HELP */
  main_stub_allocated_state_verbose = 1;
  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT(strstr(cap.data, "General options:") != NULL);
  UT_ASSERT(strstr(cap.data, "Special options:") != NULL);
  UT_ASSERT_EQ(main_stub_last_return, 0);
  free_captured_stdout(&cap);
  main_stub_allocated_state_verbose = 0;
  return 0;
}

UT_TEST(test_main_compiles_single_file_to_exe)
{
  char *argv[] = {"tcc", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};
  struct filespec *f;
  size_t fsize;

  fsize = sizeof(struct filespec) + strlen("test.c");
  f = (struct filespec *)tcc_mallocz(fsize + 1);
  f->type = 0;
  memcpy(f->name, "test.c", strlen("test.c") + 1);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  tcc_free(f);
  return 0;
}

UT_TEST(test_main_no_input_files_returns_error)
{
  char *argv[] = {"tcc", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {1, argv};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 0;
  main_stub_allocated_state_nb_errors = 1;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 1);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_nb_errors = 0;
  return 0;
}

UT_TEST(test_main_obj_with_libraries_returns_error)
{
  char *argv[] = {"tcc", "-c", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_nb_libraries = 1;
  main_stub_allocated_state_nb_errors = 1;
  main_stub_allocated_state_output_type = TCC_OUTPUT_OBJ;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 1);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_nb_libraries = 0;
  main_stub_allocated_state_nb_errors = 0;
  return 0;
}

/* ========================================================================
 * tcc_is_64bit_operand
 * ======================================================================== */

UT_TEST(test_is_64bit_operand_null_is_false)
{
  UT_ASSERT_EQ(tcc_is_64bit_operand(NULL), 0);
  return 0;
}

UT_TEST(test_is_64bit_operand_int_is_false)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_INT;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 0);
  return 0;
}

UT_TEST(test_is_64bit_operand_llong_is_true)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_LLONG;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 1);
  return 0;
}

UT_TEST(test_is_64bit_operand_double_is_true)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_DOUBLE;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 1);
  return 0;
}

UT_TEST(test_is_64bit_operand_long_double_is_true)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_LDOUBLE;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 1);
  return 0;
}

UT_TEST(test_is_64bit_operand_ignores_non_btype_bits)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  /* VT_UNSIGNED is not a basic type; only the BTYPE matters. */
  sv.type.t = VT_INT | VT_UNSIGNED;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 0);

  sv.type.t = VT_LLONG | VT_UNSIGNED;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 1);
  return 0;
}

/* ========================================================================
 * default_outputfile
 * ======================================================================== */

UT_TEST(test_default_outputfile_falls_back_to_a_out)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_EXE;

  char *out = default_outputfile(&s, NULL);
  UT_ASSERT_STREQ(out, "a.out");
  tcc_free(out);
  return 0;
}

UT_TEST(test_default_outputfile_uses_basename_for_obj)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_OBJ;

  char *out = default_outputfile(&s, "/path/to/source.c");
  UT_ASSERT_STREQ(out, "source.o");
  tcc_free(out);
  return 0;
}

UT_TEST(test_default_outputfile_preserves_leading_underscore_for_obj)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_OBJ;

  char *out = default_outputfile(&s, "_secret.c");
  UT_ASSERT_STREQ(out, "_secret.o");
  tcc_free(out);
  return 0;
}

UT_TEST(test_default_outputfile_exe_overwrites_extension_with_a_out)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_EXE;

  char *out = default_outputfile(&s, "program.c");
  UT_ASSERT_STREQ(out, "a.out");
  tcc_free(out);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(tcc)
{
  UT_RUN(test_print_dirs_shows_dash_for_empty_list);
  UT_RUN(test_print_dirs_lists_each_path);
  UT_RUN(test_print_search_dirs_outputs_all_sections);
  UT_RUN(test_set_environment_picks_up_c_include_path);
  UT_RUN(test_set_environment_picks_up_cpath);
  UT_RUN(test_set_environment_picks_up_library_path);
  UT_RUN(test_set_environment_handles_all_three_variables);

  UT_RUN(test_main_help_returns_zero);
  UT_RUN(test_main_version_returns_zero);
  UT_RUN(test_main_parse_failure_returns_one);
  UT_RUN(test_main_verbose_version_prints_version);
  UT_RUN(test_main_verbose_help_prints_both_helps);
  UT_RUN(test_main_compiles_single_file_to_exe);
  UT_RUN(test_main_no_input_files_returns_error);
  UT_RUN(test_main_obj_with_libraries_returns_error);

  UT_RUN(test_is_64bit_operand_null_is_false);
  UT_RUN(test_is_64bit_operand_int_is_false);
  UT_RUN(test_is_64bit_operand_llong_is_true);
  UT_RUN(test_is_64bit_operand_double_is_true);
  UT_RUN(test_is_64bit_operand_long_double_is_true);
  UT_RUN(test_is_64bit_operand_ignores_non_btype_bits);

  UT_RUN(test_default_outputfile_falls_back_to_a_out);
  UT_RUN(test_default_outputfile_uses_basename_for_obj);
  UT_RUN(test_default_outputfile_preserves_leading_underscore_for_obj);
  UT_RUN(test_default_outputfile_exe_overwrites_extension_with_a_out);
}
