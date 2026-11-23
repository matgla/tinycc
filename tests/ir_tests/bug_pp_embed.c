/*
 *  TCC preprocessor - C23 #embed and __has_embed
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* #embed was an unknown directive: a warning, and the initializer stayed
 * empty.  It now expands to the file's bytes as int constants, with the
 * limit / prefix / suffix / if_empty parameters, and __has_embed. */

#include <stdio.h>

static const unsigned char hello[] = {
#embed "embed/hello.txt"
};

static const unsigned char bytes[] = {
#embed "embed/bytes.bin"
};

static const unsigned char limited[] = {
#embed "embed/hello.txt" limit(5)
};

#define LIM 3
static const unsigned char limited_macro[] = {
#embed "embed/hello.txt" limit(LIM + 1)
};

static const char str[] = {
#embed "embed/hello.txt" limit(5) suffix(, 0)
};

static const int wrapped[] = {
#embed "embed/hello.txt" limit(2) prefix(7, ) suffix(, 9) if_empty(42)
};

static const int empty_file[] = {
#embed "embed/empty.bin" prefix(1, ) suffix(, 2) if_empty(42)
};

static const int empty_limit[] = {
#embed "embed/hello.txt" limit(0) prefix(1, ) if_empty(-1, -2)
};

static const int std_spelling[] = {
#embed "embed/hello.txt" __limit__(1) __suffix__(, 5)
};

#define HELLO "embed/hello.txt"
static const unsigned char computed[] = {
#embed HELLO limit(2)
};

#if !defined(__has_embed)
#error "__has_embed not defined"
#endif
#if __has_embed("embed/hello.txt") != __STDC_EMBED_FOUND__
#error "hello.txt not found"
#endif
#if __has_embed("embed/empty.bin") != __STDC_EMBED_EMPTY__
#error "empty.bin not empty"
#endif
#if __has_embed("embed/hello.txt" limit(0)) != __STDC_EMBED_EMPTY__
#error "limit(0) not empty"
#endif
#if __has_embed("embed/missing.bin") != __STDC_EMBED_NOT_FOUND__
#error "missing.bin found"
#endif
#if __has_embed("embed/hello.txt" no_such_param(1)) != __STDC_EMBED_NOT_FOUND__
#error "unknown parameter accepted"
#endif
#if __has_embed("embed/hello.txt" prefix(a) suffix(b) if_empty(c)) != __STDC_EMBED_FOUND__
#error "standard parameters rejected"
#endif
/* A computed header name used to leave numbers unconverted for the rest of
   the #if: "bad preprocessor expression". */
#define STDIO_H <stdio.h>
#if !(__has_include(STDIO_H) && __has_embed(HELLO) == 1)
#error "computed header name broke the rest of #if"
#endif

static int sum3(int a, int b, int c)
{
  return a + b + c;
}

static void dump_int(const char *name, const int *v, int n)
{
  printf("%s[%d]:", name, n);
  for (int i = 0; i < n; i++)
    printf(" %d", v[i]);
  printf("\n");
}

int main(void)
{
  int first = (
#embed "embed/hello.txt" limit(1)
  );
  int args = sum3(
#embed "embed/hello.txt" limit(3)
  );
  unsigned sum = 0;
  int ok = 1;

  for (int i = 0; i < (int)sizeof bytes; i++)
  {
    sum += bytes[i];
    if (bytes[i] != i)
      ok = 0;
  }
  printf("hello[%d]: %.*s", (int)sizeof hello, (int)sizeof hello, (const char *)hello);
  printf("bytes[%d]: ordered=%d sum=%u last=%d\n", (int)sizeof bytes, ok, sum, bytes[255]);
  printf("limited[%d]: %.*s\n", (int)sizeof limited, (int)sizeof limited, (const char *)limited);
  printf("limited_macro[%d]: %.*s\n", (int)sizeof limited_macro, (int)sizeof limited_macro,
         (const char *)limited_macro);
  printf("str[%d]: %s\n", (int)sizeof str, str);
  dump_int("wrapped", wrapped, (int)(sizeof wrapped / sizeof wrapped[0]));
  dump_int("empty_file", empty_file, (int)(sizeof empty_file / sizeof empty_file[0]));
  dump_int("empty_limit", empty_limit, (int)(sizeof empty_limit / sizeof empty_limit[0]));
  dump_int("std_spelling", std_spelling, (int)(sizeof std_spelling / sizeof std_spelling[0]));
  printf("computed[%d]: %c%c\n", (int)sizeof computed, computed[0], computed[1]);
  printf("first=%d args=%d\n", first, args);
  return 0;
}
