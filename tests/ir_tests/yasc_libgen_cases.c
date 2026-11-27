/*
 *  TCC - YasOS libc's libgen.c (yasc slices) on QEMU
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* test_yasc_libgen.py links libs/libc/libgen.c (built by tcc with libc's
 * headers) to this newlib program: dirname/basename against POSIX.1-2024
 * (XSH basename(), dirname(): DESCRIPTION, APPLICATION USAGE and the EXAMPLES
 * table) and glibc's answers.  argv[1] "chk" calls dirname on a buffer with no
 * NUL: -ffortify's check at the call (libgen.h's null_terminated_string_arg)
 * ends in __chk_fail (exit 3). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

/* as libgen.h declares them */
char *dirname(char *path) __attribute__((null_terminated_string_arg(1)));
char *basename(char *path) __attribute__((null_terminated_string_arg(1)));

void __chk_fail(void)
{
  puts("chk_fail");
  exit(3);
}

static int bad;

static void check(const char *what, const char *in, const char *got, const char *want)
{
  if (strcmp(got, want) != 0)
  {
    printf("%s(\"%s\"): \"%s\", want \"%s\"\n", what, in, got, want);
    bad++;
  }
}

/* POSIX leaves a choice: "//" may name something other than "/" (XBD 4.16),
   and dirname may drop redundant '/' */
static void check2(const char *what, const char *in, const char *got, const char *want, const char *or_want)
{
  if (strcmp(got, want) != 0 && strcmp(got, or_want) != 0)
  {
    printf("%s(\"%s\"): \"%s\", want \"%s\" or \"%s\"\n", what, in, got, want, or_want);
    bad++;
  }
}

int main(int argc, char **argv)
{
  static const char *const D[][2] = {{"usr/lib", "usr"}, {"/usr/lib", "/usr"}, {"/usr/", "/"}, {"usr", "."},
                                     {"/", "/"}, {"", "."}, {".", "."}, {"..", "."}, {"a/b/", "a"},
                                     {"a//b", "a"}, {"/a", "/"}, {"a/", "."}, {"./configure", "."},
                                     {"../x/y//", "../x"}, {"//", "/"}};
  static const char *const B[][2] = {{"usr/lib", "lib"}, {"/usr/lib", "lib"}, {"/usr/", "usr"}, {"usr", "usr"},
                                     {"/", "/"}, {"", "."}, {".", "."}, {"..", ".."}, {"a/b/", "b"},
                                     {"a//b", "b"}, {"/a", "a"}, {"a/", "a"}, {"./configure", "configure"},
                                     {"../x/y//", "y"}, {"///", "/"}, {"a///", "a"}};
  char buf[64];
  if (argc > 1 && !strcmp(argv[1], "chk"))
  {
    char nonul[8];
    memset(nonul, 'a', sizeof nonul);
    dirname(nonul);
    puts("not caught");
    return 1;
  }
  for (size_t i = 0; i < sizeof D / sizeof D[0]; i++)
  {
    strlcpy(buf, D[i][0], sizeof buf);
    check("dirname", D[i][0], dirname(buf), D[i][1]);
  }
  for (size_t i = 0; i < sizeof B / sizeof B[0]; i++)
  {
    strlcpy(buf, B[i][0], sizeof buf);
    check("basename", B[i][0], basename(buf), B[i][1]);
  }
  check("dirname", "NULL", dirname(NULL), ".");
  check("basename", "NULL", basename(NULL), ".");
  /* the EXAMPLES table of POSIX.1-2024 basename() and dirname() */
  static const char *const PD[][3] = {
      {"usr", ".", "."},
      {"usr/", ".", "."},
      {"", ".", "."},
      {"/", "/", "/"},
      {"//", "/", "//"},
      {"///", "/", "///"},
      {"/usr/", "/", "/"},
      {"/usr/lib", "/usr", "/usr"},
      {"//usr//lib//", "//usr", "/usr"},
      {"/home//dwc//test", "/home//dwc", "/home/dwc"},
      {"/home/.././test", "/home/../.", "/home/.."}, /* ".." stays: no pathname resolution */
      {"/home/dwc/.", "/home/dwc", "/home/dwc"},
      {"//foo", "//", "/"}, /* APPLICATION USAGE: "//" or "/", nothing else */
  };
  static const char *const PB[][3] = {
      {"usr", "usr", "usr"},
      {"usr/", "usr", "usr"},
      {"", ".", "."},
      {"/", "/", "/"},
      {"//", "/", "//"},
      {"///", "/", "/"},
      {"/usr/", "usr", "usr"},
      {"/usr/lib", "lib", "lib"},
      {"//usr//lib//", "lib", "lib"},
      {"/home//dwc//test", "test", "test"},
      {"/home/.././test", "test", "test"},
      {"/home/dwc/.", ".", "."},
  };
  for (size_t i = 0; i < sizeof PD / sizeof PD[0]; i++)
  {
    strlcpy(buf, PD[i][0], sizeof buf);
    check2("dirname", PD[i][0], dirname(buf), PD[i][1], PD[i][2]);
  }
  for (size_t i = 0; i < sizeof PB / sizeof PB[0]; i++)
  {
    strlcpy(buf, PB[i][0], sizeof buf);
    check2("basename", PB[i][0], basename(buf), PB[i][1], PB[i][2]);
  }

  /* POSIX sets no length limit (no pathname resolution, "always successful",
     no errors defined): a string longer than PATH_MAX (128) has an answer too */
  static char longp[300], want[300];
  memset(longp, 'b', sizeof longp - 1);
  memcpy(longp, "/x/", 3);
  memcpy(longp + sizeof longp - 5, "/c//", 4); /* 299 characters */
  memcpy(want, longp, sizeof longp - 5);       /* "/x/bbb...b" */
  check("dirname", "long", dirname(longp), want);
  memset(longp, 'b', sizeof longp - 1);
  memcpy(longp, "/x/", 3);
  memcpy(longp + sizeof longp - 5, "/c//", 4);
  check("basename", "long", basename(longp), "c");
  memset(longp, 'b', sizeof longp - 1); /* one component, no '/' */
  check("dirname", "long-name", dirname(longp), ".");
  memset(longp, 'b', sizeof longp - 1);
  if (strlen(basename(longp)) != sizeof longp - 1)
    bad++, puts("basename(long-name): not the whole name");
  memset(longp, 'b', sizeof longp - 1);
  longp[127] = 0; /* 127 characters: a path */
  longp[3] = '/';
  check("dirname", "127", dirname(longp), "bbb");
  printf("%s %d\n", bad ? "FAIL" : "PASS", bad);
  return bad != 0;
}
