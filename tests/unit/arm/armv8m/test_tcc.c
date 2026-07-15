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

static int main_stub_allocated_state_just_deps = 0;
static int main_stub_allocated_state_option_r = 0;
static unsigned char main_stub_allocated_state_dflag = 0;
static unsigned char main_stub_allocated_state_do_bench = 0;
static int main_stub_allocated_state_run_test = 0;
static char *main_stub_allocated_state_outfile = NULL;
static char *main_stub_allocated_state_tcc_lib_path = NULL;
static char **main_stub_allocated_state_sysinclude_paths = NULL;
static int main_stub_allocated_state_nb_sysinclude_paths = 0;
static char **main_stub_allocated_state_library_paths = NULL;
static int main_stub_allocated_state_nb_library_paths = 0;
static char **main_stub_allocated_state_crt_paths = NULL;
static int main_stub_allocated_state_nb_crt_paths = 0;
static struct filespec **main_stub_allocated_state_files = NULL;

/* Controls for dependency-generation paths. */
static unsigned char main_stub_allocated_state_gen_deps = 0;
static unsigned char main_stub_allocated_state_gen_phony_deps = 0;
static char **main_stub_allocated_state_target_deps = NULL;
static int main_stub_allocated_state_nb_target_deps = 0;
static char *main_stub_allocated_state_deps_outfile = NULL;

/* Controls for group-rescan behaviour. */
static int main_stub_group_has_satisfiable_undefs_ret = 0;
static int main_stub_add_library_set_new_undef = 0;
static int main_stub_add_library_set_group_rescan_loaded = 0;
static int main_stub_add_library_call_count = 0;

/* Controls for file-add failures and rescan of .a archives. */
static int main_stub_add_file_ret = 0;
static int main_stub_add_file_set_group_rescan_loaded = 0;

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
    s->files = main_stub_allocated_state_files
                   ? main_stub_allocated_state_files
                   : &main_stub_allocated_state_filespec;
    s->output_type = main_stub_allocated_state_output_type;
    s->outfile = main_stub_allocated_state_outfile;
    s->dflag = main_stub_allocated_state_dflag;
    s->do_bench = main_stub_allocated_state_do_bench;
    s->run_test = main_stub_allocated_state_run_test;
    s->just_deps = main_stub_allocated_state_just_deps;
    s->option_r = main_stub_allocated_state_option_r;
    s->sysinclude_paths = main_stub_allocated_state_sysinclude_paths;
    s->nb_sysinclude_paths = main_stub_allocated_state_nb_sysinclude_paths;
    s->library_paths = main_stub_allocated_state_library_paths;
    s->nb_library_paths = main_stub_allocated_state_nb_library_paths;
    s->crt_paths = main_stub_allocated_state_crt_paths;
    s->nb_crt_paths = main_stub_allocated_state_nb_crt_paths;
    s->tcc_lib_path = main_stub_allocated_state_tcc_lib_path;
    s->gen_deps = main_stub_allocated_state_gen_deps;
    s->gen_phony_deps = main_stub_allocated_state_gen_phony_deps;
    s->target_deps = main_stub_allocated_state_target_deps;
    s->nb_target_deps = main_stub_allocated_state_nb_target_deps;
    s->deps_outfile = main_stub_allocated_state_deps_outfile;
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
  (void)libraryname;
  main_stub_add_library_call_count++;
  if (main_stub_add_library_set_new_undef && main_stub_add_library_call_count == 1)
    s->new_undef_sym = 1;
  if (main_stub_add_library_set_group_rescan_loaded && main_stub_add_library_call_count == 2)
    s->group_rescan_loaded = 1;
  return 0;
}

int tcc_add_file(TCCState *s, const char *filename)
{
  if (main_stub_add_file_ret != 0)
  {
    s->nb_errors++;
    return main_stub_add_file_ret;
  }
  if (main_stub_add_file_set_group_rescan_loaded && filename)
  {
    size_t len = strlen(filename);
    if (len > 2 && strcmp(filename + len - 2, ".a") == 0)
      s->group_rescan_loaded = 1;
  }
  return 0;
}

int tcc_group_has_satisfiable_undefs(TCCState *s1)
{
  (void)s1;
  return main_stub_group_has_satisfiable_undefs_ret;
}

int tcc_output_file(TCCState *s, const char *filename)
{
  (void)s;
  (void)filename;
  return 0;
}

static struct filespec *make_filespec(const char *name, int type)
{
  size_t n = strlen(name);
  struct filespec *f = (struct filespec *)tcc_mallocz(sizeof(struct filespec) + n + 1);
  f->type = type;
  memcpy(f->name, name, n + 1);
  return f;
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

/* ========================================================================
 * default_outputfile (additional branches)
 * ======================================================================== */

UT_TEST(test_default_outputfile_just_deps_changes_ext_to_o)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_EXE;
  s.just_deps = 1;

  char *out = default_outputfile(&s, "source.c");
  UT_ASSERT_STREQ(out, "source.o");
  tcc_free(out);
  return 0;
}

UT_TEST(test_default_outputfile_option_r_falls_back_to_a_out)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_OBJ;
  s.option_r = 1;

  char *out = default_outputfile(&s, "source.c");
  UT_ASSERT_STREQ(out, "a.out");
  tcc_free(out);
  return 0;
}

UT_TEST(test_default_outputfile_no_extension_falls_back_to_a_out)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_OBJ;

  char *out = default_outputfile(&s, "source");
  UT_ASSERT_STREQ(out, "a.out");
  tcc_free(out);
  return 0;
}

UT_TEST(test_default_outputfile_stdin_input_falls_back_to_a_out)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_OBJ;

  char *out = default_outputfile(&s, "-");
  UT_ASSERT_STREQ(out, "a.out");
  tcc_free(out);
  return 0;
}

/* ========================================================================
 * tcc_is_64bit_operand (additional branch)
 * ======================================================================== */

UT_TEST(test_is_64bit_operand_float_is_false)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_FLOAT;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 0);
  return 0;
}

/* ========================================================================
 * main() additional early-exit and file-processing paths
 * ======================================================================== */

UT_TEST(test_main_help2_returns_zero)
{
  char *argv[] = {"tcc", "-hh", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};

  main_stub_parse_args_ret = OPT_HELP2;
  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT(strstr(cap.data, "More Options") != NULL);
  UT_ASSERT_EQ(main_stub_last_return, 0);
  free_captured_stdout(&cap);
  return 0;
}

UT_TEST(test_main_print_search_dirs_returns_zero)
{
  char *argv[] = {"tcc", "-print-search-dirs", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};
  char tcc_lib_path[] = "/opt/tcc";
  char inc1[] = "/usr/include";
  char *sysincludes[] = {inc1};
  char lib1[] = "/usr/lib";
  char *libraries[] = {lib1};
  char crt1[] = "/usr/lib/crt1.o";
  char *crts[] = {crt1};

  main_stub_parse_args_ret = OPT_PRINT_DIRS;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_tcc_lib_path = tcc_lib_path;
  main_stub_allocated_state_sysinclude_paths = sysincludes;
  main_stub_allocated_state_nb_sysinclude_paths = 1;
  main_stub_allocated_state_library_paths = libraries;
  main_stub_allocated_state_nb_library_paths = 1;
  main_stub_allocated_state_crt_paths = crts;
  main_stub_allocated_state_nb_crt_paths = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT(strstr(cap.data, "install: /opt/tcc") != NULL);
  UT_ASSERT(strstr(cap.data, "include:\n  /usr/include") != NULL);
  UT_ASSERT(strstr(cap.data, "libraries:\n  /usr/lib") != NULL);
  UT_ASSERT(strstr(cap.data, "crt:\n  /usr/lib/crt1.o") != NULL);
  UT_ASSERT(strstr(cap.data, "elfinterp:\n  /lib/ld-linux-armhf.so.3") != NULL);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_tcc_lib_path = NULL;
  main_stub_allocated_state_sysinclude_paths = NULL;
  main_stub_allocated_state_library_paths = NULL;
  main_stub_allocated_state_crt_paths = NULL;
  return 0;
}

UT_TEST(test_main_preprocess_with_outfile_dash_uses_stdout)
{
  char *argv[] = {"tcc", "-E", "-", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};
  struct filespec *f;

  f = make_filespec("-", 0);
  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_outfile = "-";
  main_stub_allocated_state_output_type = TCC_OUTPUT_PREPROCESS;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f);
  return 0;
}

UT_TEST(test_main_preprocess_with_outfile_opens_file)
{
  char *argv[] = {"tcc", "-E", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};
  struct filespec *f;
  char path[] = "/tmp/tcc_ut_ppoutXXXXXX";
  int fd;
  FILE *fp;

  fd = mkstemp(path);
  UT_ASSERT_EQ(fd >= 0, 1);
  close(fd);
  unlink(path);

  f = make_filespec("test.c", 0);
  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_outfile = path;
  main_stub_allocated_state_output_type = TCC_OUTPUT_PREPROCESS;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  fp = fopen(path, "rb");
  UT_ASSERT(fp != NULL);
  if (fp)
    fclose(fp);
  unlink(path);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f);
  return 0;
}

UT_TEST(test_main_obj_many_files_with_outfile_returns_error)
{
  char *argv[] = {"tcc", "-c", "a.c", "b.c", "-o", "out.o", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {6, argv};
  struct filespec *fa = make_filespec("a.c", 0);
  struct filespec *fb = make_filespec("b.c", 0);
  struct filespec *files_arr[] = {fa, fb};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 2;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_outfile = "out.o";
  main_stub_allocated_state_output_type = TCC_OUTPUT_OBJ;
  main_stub_allocated_state_nb_errors = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 1);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_allocated_state_nb_errors = 0;
  tcc_free(fa);
  tcc_free(fb);
  return 0;
}

UT_TEST(test_main_group_start_end_processes_files)
{
  char *argv[] = {"tcc", "--start-group", "mid.c", "--end-group", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {4, argv};
  struct filespec *f_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_mid = make_filespec("mid.c", 0);
  struct filespec *f_end = make_filespec("", AFF_GROUP_END);
  struct filespec *files_arr[] = {f_start, f_mid, f_end};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 3;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_outfile = "a.out";
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f_start);
  tcc_free(f_mid);
  tcc_free(f_end);
  return 0;
}

UT_TEST(test_main_unmatched_end_group_returns_error)
{
  char *argv[] = {"tcc", "--end-group", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};
  struct filespec *f_end = make_filespec("", AFF_GROUP_END);
  struct filespec *files_arr[] = {f_end};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_allocated_state_nb_errors = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 1);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_allocated_state_nb_errors = 0;
  tcc_free(f_end);
  return 0;
}

UT_TEST(test_main_missing_end_group_returns_error)
{
  char *argv[] = {"tcc", "--start-group", "mid.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};
  struct filespec *f_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_mid = make_filespec("mid.c", 0);
  struct filespec *files_arr[] = {f_start, f_mid};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 2;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_allocated_state_nb_errors = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 1);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_allocated_state_nb_errors = 0;
  tcc_free(f_start);
  tcc_free(f_mid);
  return 0;
}

UT_TEST(test_main_run_test_dt_path_returns_zero)
{
  char *argv[] = {"tcc", "-dt", "-run", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {4, argv};
  struct filespec *f = make_filespec("test.c", 0);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_outfile = "a.out";
  main_stub_allocated_state_output_type = TCC_OUTPUT_MEMORY;
  main_stub_allocated_state_dflag = 16;
  main_stub_allocated_state_run_test = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_allocated_state_dflag = 0;
  main_stub_allocated_state_run_test = 0;
  tcc_free(f);
  return 0;
}

UT_TEST(test_main_do_bench_path_returns_zero)
{
  char *argv[] = {"tcc", "-bench", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};
  struct filespec *f = make_filespec("test.c", 0);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_outfile = "a.out";
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_allocated_state_do_bench = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_allocated_state_do_bench = 0;
  tcc_free(f);
  return 0;
}

/* ========================================================================
 * main() additional file-processing and output-type paths
 * ======================================================================== */

UT_TEST(test_main_memory_output_no_dt_returns_zero)
{
  char *argv[] = {"tcc", "-run", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};
  struct filespec *f = make_filespec("test.c", 0);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_outfile = "a.out";
  main_stub_allocated_state_output_type = TCC_OUTPUT_MEMORY;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f);
  return 0;
}

UT_TEST(test_main_library_file_returns_zero)
{
  char *argv[] = {"tcc", "-lfoo", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};
  struct filespec *f = make_filespec("foo", AFF_TYPE_LIB);

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
  main_stub_allocated_state_output_type = 0;
  tcc_free(f);
  return 0;
}

UT_TEST(test_main_verbose_prints_processed_file)
{
  char *argv[] = {"tcc", "-v", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};
  struct filespec *f = make_filespec("test.c", 0);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_allocated_state_verbose = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT(strstr(cap.data, "-> test.c") != NULL);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_allocated_state_verbose = 0;
  tcc_free(f);
  return 0;
}

UT_TEST(test_main_gen_deps_writes_dependency_file)
{
  char *argv[] = {"tcc", "-MD", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};
  struct filespec *f = make_filespec("test.c", 0);
  char dep_path[] = "/tmp/tcc_ut_depsXXXXXX";
  char *deps[] = {"test.c", "test.h"};
  int fd;
  FILE *fp;
  char buf[256];

  fd = mkstemp(dep_path);
  UT_ASSERT_EQ(fd >= 0, 1);
  close(fd);
  unlink(dep_path);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_allocated_state_outfile = "test.out";
  main_stub_allocated_state_gen_deps = 1;
  main_stub_allocated_state_target_deps = deps;
  main_stub_allocated_state_nb_target_deps = 2;
  main_stub_allocated_state_deps_outfile = dep_path;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  fp = fopen(dep_path, "rb");
  UT_ASSERT(fp != NULL);
  if (fp)
  {
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    UT_ASSERT(strstr(buf, "test.out:") != NULL);
    UT_ASSERT(strstr(buf, "test.c") != NULL);
    UT_ASSERT(strstr(buf, "test.h") != NULL);
  }
  unlink(dep_path);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_gen_deps = 0;
  main_stub_allocated_state_target_deps = NULL;
  main_stub_allocated_state_nb_target_deps = 0;
  main_stub_allocated_state_deps_outfile = NULL;
  tcc_free(f);
  return 0;
}

UT_TEST(test_main_c_multiple_files_without_option_r_redoes)
{
  char *argv[] = {"tcc", "-c", "a.c", "b.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {4, argv};
  struct filespec *fa = make_filespec("a.c", 0);
  struct filespec *fb = make_filespec("b.c", 0);
  struct filespec *files_arr[] = {fa, fb};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 2;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_OBJ;
  /* option_r stays 0, so each file is compiled in a separate redo pass. */

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(fa);
  tcc_free(fb);
  return 0;
}

UT_TEST(test_main_group_with_library_returns_zero)
{
  char *argv[] = {"tcc", "--start-group", "-lfoo", "--end-group", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {4, argv};
  struct filespec *f_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_lib = make_filespec("foo", AFF_TYPE_LIB);
  struct filespec *f_end = make_filespec("", AFF_GROUP_END);
  struct filespec *files_arr[] = {f_start, f_lib, f_end};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 3;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f_start);
  tcc_free(f_lib);
  tcc_free(f_end);
  return 0;
}

UT_TEST(test_main_group_rescan_loaded)
{
  char *argv[] = {"tcc", "--start-group", "-lfoo", "--end-group", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {4, argv};
  struct filespec *f_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_lib = make_filespec("foo", AFF_TYPE_LIB);
  struct filespec *f_end = make_filespec("", AFF_GROUP_END);
  struct filespec *files_arr[] = {f_start, f_lib, f_end};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 3;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  /* First pass adds an undef; rescan is deemed satisfiable and loads a member. */
  main_stub_add_library_call_count = 0;
  main_stub_add_library_set_new_undef = 1;
  main_stub_group_has_satisfiable_undefs_ret = 1;
  main_stub_add_library_set_group_rescan_loaded = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_add_library_set_new_undef = 0;
  main_stub_group_has_satisfiable_undefs_ret = 0;
  main_stub_add_library_set_group_rescan_loaded = 0;
  main_stub_add_library_call_count = 0;
  tcc_free(f_start);
  tcc_free(f_lib);
  tcc_free(f_end);
  return 0;
}

/* ========================================================================
 * main() tool early-exit paths
 * ======================================================================== */

UT_TEST(test_main_opt_ar_empty_archive_returns_zero)
{
  char ar_path[] = "/tmp/tcc_ut_arXXXXXX";
  int fd;
  char *argv[5];
  struct captured_stdout cap = {0};
  struct main_args args;

  fd = mkstemp(ar_path);
  UT_ASSERT_EQ(fd >= 0, 1);
  close(fd);
  unlink(ar_path);

  argv[0] = "tcc";
  argv[1] = "-cr";
  argv[2] = ar_path;
  argv[3] = NULL;
  args.argc = 3;
  args.argv = argv;

  main_stub_parse_args_ret = OPT_AR;
  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  unlink(ar_path);
  return 0;
}

UT_TEST(test_main_opt_m32_returns_one)
{
  char *argv[] = {"tcc", "-m32", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};

  main_stub_parse_args_ret = OPT_M32;
  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 1);
  free_captured_stdout(&cap);
  return 0;
}

/* ========================================================================
 * main() default output type
 * ======================================================================== */

UT_TEST(test_main_default_output_type_to_exe)
{
  char *argv[] = {"tcc", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};
  struct filespec *f = make_filespec("test.c", 0);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_output_type = 0; /* let main() default to EXE */
  main_stub_allocated_state_outfile = "a.out";

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_allocated_state_outfile = NULL;
  tcc_free(f);
  return 0;
}

/* ========================================================================
 * main() preprocess outfile failure and add_file error paths
 * ======================================================================== */

UT_TEST(test_main_preprocess_outfile_fopen_failure)
{
  char *argv[] = {"tcc", "-E", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};
  struct filespec *f;
  char dir_template[] = "/tmp/tcc_ut_ppdirXXXXXX";
  char *dir = mkdtemp(dir_template);

  UT_ASSERT(dir != NULL);

  f = make_filespec("test.c", 0);
  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_outfile = dir; /* directory -> fopen fails */
  main_stub_allocated_state_output_type = TCC_OUTPUT_PREPROCESS;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f);
  rmdir(dir);
  return 0;
}

UT_TEST(test_main_add_file_error_returns_one)
{
  char *argv[] = {"tcc", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};
  struct filespec *f = make_filespec("test.c", 0);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_add_file_ret = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 1);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_add_file_ret = 0;
  tcc_free(f);
  return 0;
}

/* ========================================================================
 * main() group parsing edge cases
 * ======================================================================== */

UT_TEST(test_main_missing_end_group_proper)
{
  char *argv[] = {"tcc", "--start-group", "mid.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};
  struct filespec *f_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_mid = make_filespec("mid.c", 0);
  struct filespec *files_arr[] = {f_start, f_mid};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 2;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT(main_stub_last_return != 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f_start);
  tcc_free(f_mid);
  return 0;
}

UT_TEST(test_main_unmatched_end_group_proper)
{
  char *argv[] = {"tcc", "--end-group", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};
  struct filespec *f_end = make_filespec("", AFF_GROUP_END);
  struct filespec *files_arr[] = {f_end};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT(main_stub_last_return != 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f_end);
  return 0;
}

UT_TEST(test_main_nested_group)
{
  char *argv[] = {"tcc", "--start-group", "--start-group", "mid.c",
                  "--end-group", "--end-group", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {6, argv};
  struct filespec *f_outer_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_inner_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_mid = make_filespec("mid.c", 0);
  struct filespec *f_inner_end = make_filespec("", AFF_GROUP_END);
  struct filespec *f_outer_end = make_filespec("", AFF_GROUP_END);
  struct filespec *files_arr[] = {f_outer_start, f_inner_start, f_mid,
                                  f_inner_end, f_outer_end};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 5;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f_outer_start);
  tcc_free(f_inner_start);
  tcc_free(f_mid);
  tcc_free(f_inner_end);
  tcc_free(f_outer_end);
  return 0;
}

UT_TEST(test_main_verbose_group_prints_file)
{
  char *argv[] = {"tcc", "-v", "--start-group", "mid.c", "--end-group", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {5, argv};
  struct filespec *f_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_mid = make_filespec("mid.c", 0);
  struct filespec *f_end = make_filespec("", AFF_GROUP_END);
  struct filespec *files_arr[] = {f_start, f_mid, f_end};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 3;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_allocated_state_verbose = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT(strstr(cap.data, "-> mid.c") != NULL);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_allocated_state_verbose = 0;
  tcc_free(f_start);
  tcc_free(f_mid);
  tcc_free(f_end);
  return 0;
}

/* ========================================================================
 * main() group rescan branches
 * ======================================================================== */

UT_TEST(test_main_group_satisfiable_undefs_false)
{
  char *argv[] = {"tcc", "--start-group", "-lfoo", "--end-group", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {4, argv};
  struct filespec *f_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_lib = make_filespec("foo", AFF_TYPE_LIB);
  struct filespec *f_end = make_filespec("", AFF_GROUP_END);
  struct filespec *files_arr[] = {f_start, f_lib, f_end};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 3;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_add_library_call_count = 0;
  main_stub_add_library_set_new_undef = 1;
  main_stub_group_has_satisfiable_undefs_ret = 0;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_add_library_set_new_undef = 0;
  main_stub_group_has_satisfiable_undefs_ret = 0;
  main_stub_add_library_call_count = 0;
  tcc_free(f_start);
  tcc_free(f_lib);
  tcc_free(f_end);
  return 0;
}

UT_TEST(test_main_group_rescan_breaks_when_nothing_loaded)
{
  char *argv[] = {"tcc", "--start-group", "-lfoo", "--end-group", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {4, argv};
  struct filespec *f_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_lib = make_filespec("foo", AFF_TYPE_LIB);
  struct filespec *f_end = make_filespec("", AFF_GROUP_END);
  struct filespec *files_arr[] = {f_start, f_lib, f_end};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 3;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_add_library_call_count = 0;
  main_stub_add_library_set_new_undef = 1;
  main_stub_group_has_satisfiable_undefs_ret = 1;
  /* group_rescan_loaded stays 0 -> break after one rescan pass */

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_add_library_set_new_undef = 0;
  main_stub_group_has_satisfiable_undefs_ret = 0;
  main_stub_add_library_call_count = 0;
  tcc_free(f_start);
  tcc_free(f_lib);
  tcc_free(f_end);
  return 0;
}

UT_TEST(test_main_group_rescan_processes_archive_file)
{
  char *argv[] = {"tcc", "--start-group", "-lfoo", "libbar.a",
                  "--end-group", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {5, argv};
  struct filespec *f_start = make_filespec("", AFF_GROUP_START);
  struct filespec *f_lib = make_filespec("foo", AFF_TYPE_LIB);
  struct filespec *f_a = make_filespec("libbar.a", 0);
  struct filespec *f_end = make_filespec("", AFF_GROUP_END);
  struct filespec *files_arr[] = {f_start, f_lib, f_a, f_end};

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 4;
  main_stub_allocated_state_files = files_arr;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_add_library_call_count = 0;
  main_stub_add_library_set_new_undef = 1;
  main_stub_group_has_satisfiable_undefs_ret = 1;
  main_stub_add_file_set_group_rescan_loaded = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_files = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_add_library_set_new_undef = 0;
  main_stub_group_has_satisfiable_undefs_ret = 0;
  main_stub_add_library_call_count = 0;
  main_stub_add_file_set_group_rescan_loaded = 0;
  tcc_free(f_start);
  tcc_free(f_lib);
  tcc_free(f_a);
  tcc_free(f_end);
  return 0;
}

/* ========================================================================
 * tcc_is_64bit_operand (additional basic types)
 * ======================================================================== */

UT_TEST(test_is_64bit_operand_pointer_is_false)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_PTR;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 0);
  return 0;
}

UT_TEST(test_is_64bit_operand_void_is_false)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_VOID;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 0);
  return 0;
}

/* ========================================================================
 * default_outputfile (additional output types)
 * ======================================================================== */

UT_TEST(test_default_outputfile_dll_falls_back_to_a_out)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_DLL;

  char *out = default_outputfile(&s, "lib.c");
  UT_ASSERT_STREQ(out, "a.out");
  tcc_free(out);
  return 0;
}

/* ========================================================================
 * main() additional early-exit and output paths
 * ======================================================================== */

UT_TEST(test_main_opt_m64_returns_one)
{
  char *argv[] = {"tcc", "-m64", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};

  main_stub_parse_args_ret = OPT_M64;
  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 1);
  free_captured_stdout(&cap);
  return 0;
}

UT_TEST(test_main_preprocess_without_outfile_uses_stdout)
{
  char *argv[] = {"tcc", "-E", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {3, argv};
  struct filespec *f = make_filespec("test.c", 0);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_output_type = TCC_OUTPUT_PREPROCESS;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f);
  return 0;
}

UT_TEST(test_main_just_deps_skips_output_file)
{
  char *argv[] = {"tcc", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};
  struct filespec *f = make_filespec("test.c", 0);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_allocated_state_outfile = "test.out";
  main_stub_allocated_state_just_deps = 1;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_output_type = 0;
  main_stub_allocated_state_outfile = NULL;
  main_stub_allocated_state_just_deps = 0;
  tcc_free(f);
  return 0;
}

UT_TEST(test_main_output_file_without_outfile_uses_default)
{
  char *argv[] = {"tcc", "test.c", NULL};
  struct captured_stdout cap = {0};
  struct main_args args = {2, argv};
  struct filespec *f = make_filespec("test.c", 0);

  main_stub_parse_args_ret = 0;
  main_stub_allocated_state_setup = 1;
  main_stub_allocated_state_nb_files = 1;
  main_stub_allocated_state_filespec = f;
  main_stub_allocated_state_output_type = TCC_OUTPUT_EXE;
  main_stub_allocated_state_outfile = NULL;

  UT_ASSERT_EQ(capture_stdout(&cap, call_tcc_ut_main, &args), 0);
  UT_ASSERT_EQ(main_stub_last_return, 0);

  free_captured_stdout(&cap);
  main_stub_allocated_state_setup = 0;
  main_stub_allocated_state_filespec = NULL;
  main_stub_allocated_state_output_type = 0;
  tcc_free(f);
  return 0;
}
