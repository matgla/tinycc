#include <stdarg.h>
extern void abort(void);
extern void main_test (void);
extern void abort (void);
int inside_main;

int
main ()
{
  inside_main = 1;
  main_test ();
  inside_main = 0;
  return 0;
}

/* When optimizing, all the constant cases should have been
   constant folded, so no calls to link_error should remain.
   In any case, link_error should not be called.  */

#ifndef __OPTIMIZE__
void
link_error (void)
{
  abort ();
}
#endif

/* Copyright (C) 2004, 2005  Free Software Foundation.

   Ensure builtin __strcat_chk performs correctly.  */

extern void abort (void);
typedef __SIZE_TYPE__ size_t;
extern size_t strlen(const char *);
extern void *memcpy (void *, const void *, size_t);
extern char *strcat (char *, const char *);
extern int memcmp (const void *, const void *, size_t);
extern char *strcpy (char *, const char *);
extern int strcmp (const char *, const char *);
extern void *memset (void *, int, size_t);
#define RESET_DST_WITH(FILLER) \
  do { memset (dst, 'X', sizeof (dst)); strcpy (dst, (FILLER)); } while (0)

#ifndef os
# define os(ptr) __builtin_object_size (ptr, 0)
#endif

/* This is one of the alternatives for object size checking.
   If dst has side-effects, size checking will never be done.  */
#undef memcpy
#define memcpy(dst, src, len) \
  __builtin___memcpy_chk (dst, src, len, os (dst))
#undef mempcpy
#define mempcpy(dst, src, len) \
  __builtin___mempcpy_chk (dst, src, len, os (dst))
#undef memmove
#define memmove(dst, src, len) \
  __builtin___memmove_chk (dst, src, len, os (dst))
#undef memset
#define memset(dst, val, len) \
  __builtin___memset_chk (dst, val, len, os (dst))
#undef strcpy
#define strcpy(dst, src) \
  __builtin___strcpy_chk (dst, src, os (dst))
#undef stpcpy
#define stpcpy(dst, src) \
  __builtin___stpcpy_chk (dst, src, os (dst))
#undef strcat
#define strcat(dst, src) \
  __builtin___strcat_chk (dst, src, os (dst))
#undef strncpy
#define strncpy(dst, src, len) \
  __builtin___strncpy_chk (dst, src, len, os (dst))
#undef stpncpy
#define stpncpy(dst, src, len) \
  __builtin___stpncpy_chk (dst, src, len, os (dst))
#undef strncat
#define strncat(dst, src, len) \
  __builtin___strncat_chk (dst, src, len, os (dst))
#undef sprintf
#define sprintf(dst, ...) \
  __builtin___sprintf_chk (dst, 0, os (dst), __VA_ARGS__)
#undef vsprintf
#define vsprintf(dst, fmt, ap) \
  __builtin___vsprintf_chk (dst, 0, os (dst), fmt, ap)
#undef snprintf
#define snprintf(dst, len, ...) \
  __builtin___snprintf_chk (dst, len, 0, os (dst), __VA_ARGS__)
#undef vsnprintf
#define vsnprintf(dst, len, fmt, ap) \
  __builtin___vsnprintf_chk (dst, len, 0, os (dst), fmt, ap)

/* Now "redefine" even builtins for the purpose of testing.  */
#undef __builtin_memcpy
#define __builtin_memcpy(dst, src, len) memcpy (dst, src, len)
#undef __builtin_mempcpy
#define __builtin_mempcpy(dst, src, len) mempcpy (dst, src, len)
#undef __builtin_memmove
#define __builtin_memmove(dst, src, len) memmove (dst, src, len)
#undef __builtin_memset
#define __builtin_memset(dst, val, len) memset (dst, val, len)
#undef __builtin_strcpy
#define __builtin_strcpy(dst, src) strcpy (dst, src)
#undef __builtin_stpcpy
#define __builtin_stpcpy(dst, src) stpcpy (dst, src)
#undef __builtin_strcat
#define __builtin_strcat(dst, src) strcat (dst, src)
#undef __builtin_strncpy
#define __builtin_strncpy(dst, src, len) strncpy (dst, src, len)
#undef __builtin_strncat
#define __builtin_strncat(dst, src, len) strncat (dst, src, len)
#undef __builtin_sprintf
#define __builtin_sprintf(dst, ...) sprintf (dst, __VA_ARGS__)
#undef __builtin_vsprintf
#define __builtin_vsprintf(dst, fmt, ap) vsprintf (dst, fmt, ap)
#undef __builtin_snprintf
#define __builtin_snprintf(dst, len, ...) snprintf (dst, len, __VA_ARGS__)
#undef __builtin_vsnprintf
#define __builtin_vsnprintf(dst, len, fmt, ap) vsnprintf (dst, len, fmt, ap)

extern void *chk_fail_buf[];
extern volatile int chk_fail_allowed, chk_calls;
extern volatile int memcpy_disallowed, mempcpy_disallowed, memmove_disallowed;
extern volatile int memset_disallowed, strcpy_disallowed, stpcpy_disallowed;
extern volatile int strncpy_disallowed, stpncpy_disallowed, strcat_disallowed;
extern volatile int strncat_disallowed, sprintf_disallowed, vsprintf_disallowed;
extern volatile int snprintf_disallowed, vsnprintf_disallowed;

/* A storage class that ensures that declarations bind locally.  We want
   to test non-static declarations where we know it is safe to do so.  */
#if __PIC__ && !__PIE__
#define LOCAL static
#else
#define LOCAL
#endif


const char s1[] = "123";
char p[32] = "";
char *s2 = "defg";
char *s3 = "FGH";
char *s4;
size_t l1 = 1;
char *s5;

void
__attribute__((noinline))
test1 (void)
{
  const char *const x1 = "hello world";
  const char *const x2 = "";
  char dst[64], *d2;

  chk_calls = 0;
  strcat_disallowed = 1;
  /* Following strcat calls should be optimized out at compile time.  */  
  RESET_DST_WITH (x1);
  if (strcat (dst, "") != dst || strcmp (dst, x1))
    abort ();
  RESET_DST_WITH (x1);
  if (strcat (dst, x2) != dst || strcmp (dst, x1))
    abort ();
  RESET_DST_WITH (x1); d2 = dst;
  if (strcat (++d2, x2) != dst+1 || d2 != dst+1 || strcmp (dst, x1))
    abort ();
  RESET_DST_WITH (x1); d2 = dst;
  if (strcat (++d2+5, x2) != dst+6 || d2 != dst+1 || strcmp (dst, x1))
    abort ();
  RESET_DST_WITH (x1); d2 = dst;
  if (strcat (++d2+5, x1+11) != dst+6 || d2 != dst+1 || strcmp (dst, x1))
    abort ();
  if (chk_calls)
    abort ();
  strcat_disallowed = 0;

  RESET_DST_WITH (x1);
  if (strcat (dst, " 1111") != dst
      || memcmp (dst, "hello world 1111\0XXX", 20))
    abort ();
  
  RESET_DST_WITH (x1);
  if (strcat (dst+5, " 2222") != dst+5
      || memcmp (dst, "hello world 2222\0XXX", 20))
    abort ();
  
  RESET_DST_WITH (x1); d2 = dst;
  if (strcat (++d2+5, " 3333") != dst+6 || d2 != dst+1
      || memcmp (dst, "hello world 3333\0XXX", 20))
    abort ();
  
  RESET_DST_WITH (x1);
  strcat (strcat (strcat (strcat (strcat (strcat (dst, ": this "), ""),
				  "is "), "a "), "test"), ".");
  if (memcmp (dst, "hello world: this is a test.\0X", 30))
    abort ();

  chk_calls = 0;
  strcat_disallowed = 1;
  /* Test at least one instance of the __builtin_ style.  We do this
     to ensure that it works and that the prototype is correct.  */
  RESET_DST_WITH (x1);
  if (__builtin_strcat (dst, "") != dst || strcmp (dst, x1))
    abort ();
  if (chk_calls)
    abort ();
  strcat_disallowed = 0;
}


/* Test whether compile time checking is done where it should
   and so is runtime object size checking.  */
void
__attribute__((noinline))
test2 (void)
{
  struct A { char buf1[10]; char buf2[10]; } a;
  char *r = l1 == 1 ? &a.buf1[5] : &a.buf2[4];
  char buf3[20];
  int i;

  /* The following calls should do runtime checking
     - source length is not known, but destination is.  */
  memset (&a, '\0', sizeof (a));
  s5 = (char *) &a;
  __asm __volatile ("" : : "r" (s5) : "memory");
  chk_calls = 0;
  strcat (a.buf1 + 2, s3 + 3);
  strcat (r, s3 + 2);
  r = l1 == 1 ? __builtin_alloca (4) : &a.buf2[7];
  memset (r, '\0', 3);
  __asm __volatile ("" : : "r" (r) : "memory");
  strcat (r, s2 + 2);
  strcat (r + 2, s3 + 3);
  r = buf3;
  for (i = 0; i < 4; ++i)
    {
      if (i == l1 - 1)
	r = &a.buf1[1];
      else if (i == l1)
	r = &a.buf2[7];
      else if (i == l1 + 1)
	r = &buf3[5];
      else if (i == l1 + 2)
	r = &a.buf1[9];
    }
  strcat (r, s2 + 4);
  if (chk_calls != 5)
    abort ();

  /* Following have known destination and known source length,
     but we don't know the length of dest string, so runtime checking
     is needed too.  */
  memset (&a, '\0', sizeof (a));
  chk_calls = 0;
  s5 = (char *) &a;
  __asm __volatile ("" : : "r" (s5) : "memory");
  strcat (a.buf1 + 2, "a");
  strcat (r, "");
  r = l1 == 1 ? __builtin_alloca (4) : &a.buf2[7];
  memset (r, '\0', 3);
  __asm __volatile ("" : : "r" (r) : "memory");
  strcat (r, s1 + 1);
  if (chk_calls != 2)
    abort ();
  chk_calls = 0;
  /* Unknown destination and source, no checking.  */
  strcat (s4, s3);
  if (chk_calls)
    abort ();
  chk_calls = 0;
}

/* Test whether runtime and/or compile time checking catches
   buffer overflows.  */
void
__attribute__((noinline))
test3 (void)
{
  struct A { char buf1[10]; char buf2[10]; } a;
  char buf3[20];

  memset (&a, '\0', sizeof (a));
  memset (buf3, '\0', sizeof (buf3));
  s5 = (char *) &a;
  __asm __volatile ("" : : "r" (s5) : "memory");
  s5 = buf3;
  __asm __volatile ("" : : "r" (s5) : "memory");
  chk_fail_allowed = 1;
  /* Runtime checks.  */
  if (__builtin_setjmp (chk_fail_buf) == 0)
    {
      strcat (&a.buf2[9], s2 + 3);
      abort ();
    }
  if (__builtin_setjmp (chk_fail_buf) == 0)
    {
      strcat (&a.buf2[7], s3 + strlen (s3) - 3);
      abort ();
    }
  if (__builtin_setjmp (chk_fail_buf) == 0)
    {
      strcat (&buf3[19], "a");
      abort ();
    }
  chk_fail_allowed = 0;
}

void
main_test (void)
{
#ifndef __OPTIMIZE__
  /* Object size checking is only intended for -O[s123].  */
  return;
#endif
  __asm ("" : "=r" (s2) : "0" (s2));
  __asm ("" : "=r" (s3) : "0" (s3));
  __asm ("" : "=r" (l1) : "0" (l1));
  s4 = p;
  test1 ();
  memset (p, '\0', sizeof (p));
  test2 ();
  test3 ();
}


#ifdef __unix__
#include <sys/types.h>
#endif

/* If some target has a Max alignment less than 16, please create
   a #ifdef around the alignment and add your alignment.  */
#ifdef __pdp11__
#define ALIGNMENT 2
#else
#define ALIGNMENT 16
#endif

extern void abort (void);

extern int inside_main;
void *chk_fail_buf[256] __attribute__((aligned (ALIGNMENT)));
volatile int chk_fail_allowed, chk_calls;
volatile int memcpy_disallowed, mempcpy_disallowed, memmove_disallowed;
volatile int memset_disallowed, strcpy_disallowed, stpcpy_disallowed;
volatile int strncpy_disallowed, stpncpy_disallowed, strcat_disallowed;
volatile int strncat_disallowed, sprintf_disallowed, vsprintf_disallowed;
volatile int snprintf_disallowed, vsnprintf_disallowed;
extern __SIZE_TYPE__ strlen (const char *);
extern int vsprintf (char *, const char *, va_list);

void __attribute__((noreturn))
__chk_fail (void)
{
  if (chk_fail_allowed)
    __builtin_longjmp (chk_fail_buf, 1);
  abort ();
}

void *
memcpy (void *dst, const void *src, __SIZE_TYPE__ n)
{
  const char *srcp;
  char *dstp;

#ifdef __OPTIMIZE__
  if (memcpy_disallowed && inside_main)
    abort ();
#endif

  srcp = src;
  dstp = dst;
  while (n-- != 0)
    *dstp++ = *srcp++;

  return dst;
}

void *
__memcpy_chk (void *dst, const void *src, __SIZE_TYPE__ n, __SIZE_TYPE__ size)
{
  /* If size is -1, GCC should always optimize the call into memcpy.  */
  if (size == (__SIZE_TYPE__) -1)
    abort ();
  ++chk_calls;
  if (n > size)
    __chk_fail ();
  return memcpy (dst, src, n);
}

void *
mempcpy (void *dst, const void *src, __SIZE_TYPE__ n)
{
  const char *srcp;
  char *dstp;

#ifdef __OPTIMIZE__
  if (mempcpy_disallowed && inside_main)
    abort ();
#endif

  srcp = src;
  dstp = dst;
  while (n-- != 0)
    *dstp++ = *srcp++;

  return dstp;
}

void *
__mempcpy_chk (void *dst, const void *src, __SIZE_TYPE__ n, __SIZE_TYPE__ size)
{
  /* If size is -1, GCC should always optimize the call into mempcpy.  */
  if (size == (__SIZE_TYPE__) -1)
    abort ();
  ++chk_calls;
  if (n > size)
    __chk_fail ();
  return mempcpy (dst, src, n);
}

void *
memmove (void *dst, const void *src, __SIZE_TYPE__ n)
{
  const char *srcp;
  char *dstp;

#ifdef __OPTIMIZE__
  if (memmove_disallowed && inside_main)
    abort ();
#endif

  srcp = src;
  dstp = dst;
  if (srcp < dstp)
    while (n-- != 0)
      dstp[n] = srcp[n];
  else
    while (n-- != 0)
      *dstp++ = *srcp++;

  return dst;
}

void *
__memmove_chk (void *dst, const void *src, __SIZE_TYPE__ n, __SIZE_TYPE__ size)
{
  /* If size is -1, GCC should always optimize the call into memmove.  */
  if (size == (__SIZE_TYPE__) -1)
    abort ();
  ++chk_calls;
  if (n > size)
    __chk_fail ();
  return memmove (dst, src, n);
}

void *
memset (void *dst, int c, __SIZE_TYPE__ n)
{
  while (n-- != 0)
    n[(char *) dst] = c;

  /* Single-byte memsets should be done inline when optimisation
     is enabled.  Do this after the copy in case we're being called to
     initialize bss.  */
#ifdef __OPTIMIZE__
  if (memset_disallowed && inside_main && n < 2)
    abort ();
#endif

  return dst;
}

void *
__memset_chk (void *dst, int c, __SIZE_TYPE__ n, __SIZE_TYPE__ size)
{
  /* If size is -1, GCC should always optimize the call into memset.  */
  if (size == (__SIZE_TYPE__) -1)
    abort ();
  ++chk_calls;
  if (n > size)
    __chk_fail ();
  return memset (dst, c, n);
}

char *
strcpy (char *d, const char *s)
{
  char *r = d;
#ifdef __OPTIMIZE__
  if (strcpy_disallowed && inside_main)
    abort ();
#endif
  while ((*d++ = *s++));
  return r;
}

char *
__strcpy_chk (char *d, const char *s, __SIZE_TYPE__ size)
{
  /* If size is -1, GCC should always optimize the call into strcpy.  */
  if (size == (__SIZE_TYPE__) -1)
    abort ();
  ++chk_calls;
  if (strlen (s) >= size)
    __chk_fail ();
  return strcpy (d, s);
}

char *
stpcpy (char *dst, const char *src)
{
#ifdef __OPTIMIZE__
  if (stpcpy_disallowed && inside_main)
    abort ();
#endif

  while (*src != 0)
    *dst++ = *src++;

  *dst = 0;
  return dst;
}

char *
__stpcpy_chk (char *d, const char *s, __SIZE_TYPE__ size)
{
  /* If size is -1, GCC should always optimize the call into stpcpy.  */
  if (size == (__SIZE_TYPE__) -1)
    abort ();
  ++chk_calls;
  if (strlen (s) >= size)
    __chk_fail ();
  return stpcpy (d, s);
}

char *
stpncpy (char *dst, const char *src, __SIZE_TYPE__ n)
{
#ifdef __OPTIMIZE__
  if (stpncpy_disallowed && inside_main)
    abort ();
#endif

  for (; *src && n; n--)
    *dst++ = *src++;

  char *ret = dst;

  while (n--)
    *dst++ = 0;

  return ret;
}


char *
__stpncpy_chk (char *s1, const char *s2, __SIZE_TYPE__ n, __SIZE_TYPE__ size)
{
  /* If size is -1, GCC should always optimize the call into stpncpy.  */
  if (size == (__SIZE_TYPE__) -1)
    abort ();
  ++chk_calls;
  if (n > size)
    __chk_fail ();
  return stpncpy (s1, s2, n);
}

char *
strncpy (char *s1, const char *s2, __SIZE_TYPE__ n)
{
  char *dest = s1;
#ifdef __OPTIMIZE__
  if (strncpy_disallowed && inside_main)
    abort();
#endif
  for (; *s2 && n; n--)
    *s1++ = *s2++;
  while (n--)
    *s1++ = 0;
  return dest;
}

char *
__strncpy_chk (char *s1, const char *s2, __SIZE_TYPE__ n, __SIZE_TYPE__ size)
{
  /* If size is -1, GCC should always optimize the call into strncpy.  */
  if (size == (__SIZE_TYPE__) -1)
    abort ();
  ++chk_calls;
  if (n > size)
    __chk_fail ();
  return strncpy (s1, s2, n);
}

char *
strcat (char *dst, const char *src)
{
  char *p = dst;
  
#ifdef __OPTIMIZE__
  if (strcat_disallowed && inside_main)
    abort ();
#endif

  while (*p)
    p++;
  while ((*p++ = *src++))
    ;
  return dst;
}

char *
__strcat_chk (char *d, const char *s, __SIZE_TYPE__ size)
{
  /* If size is -1, GCC should always optimize the call into strcat.  */
  if (size == (__SIZE_TYPE__) -1)
    abort ();
  ++chk_calls;
  if (strlen (d) + strlen (s) >= size)
    __chk_fail ();
  return strcat (d, s);
}

char *
strncat (char *s1, const char *s2, __SIZE_TYPE__ n)
{
  char *dest = s1;
  char c;
#ifdef __OPTIMIZE__
  if (strncat_disallowed && inside_main)
    abort();
#endif
  while (*s1) s1++;
  c = '\0';
  while (n > 0)
    {
      c = *s2++;
      *s1++ = c;
      if (c == '\0')
	return dest;
      n--;
    }
  if (c != '\0')
    *s1 = '\0';
  return dest;
}

char *
__strncat_chk (char *d, const char *s, __SIZE_TYPE__ n, __SIZE_TYPE__ size)
{
  __SIZE_TYPE__ len = strlen (d), n1 = n;
  const char *s1 = s;

  /* If size is -1, GCC should always optimize the call into strncat.  */
  if (size == (__SIZE_TYPE__) -1)
    abort ();
  ++chk_calls;
  while (len < size && n1 > 0)
    {
      if (*s1++ == '\0')
	break;
      ++len;
      --n1;
    }

  if (len >= size)
    __chk_fail ();
  return strncat (d, s, n);
}

/* No chk test in GCC testsuite needs more bytes than this.
   As we can't expect vsnprintf to be available on the target,
   assume 4096 bytes is enough.  */
static char chk_sprintf_buf[4096];

int
__sprintf_chk (char *str, int flag, __SIZE_TYPE__ size, const char *fmt, ...)
{
  int ret;
  va_list ap;

  /* If size is -1 and flag 0, GCC should always optimize the call into
     sprintf.  */
  if (size == (__SIZE_TYPE__) -1 && flag == 0)
    abort ();
  ++chk_calls;
#ifdef __OPTIMIZE__
  if (sprintf_disallowed && inside_main)
    abort();
#endif
  va_start (ap, fmt);
  ret = vsprintf (chk_sprintf_buf, fmt, ap);
  va_end (ap);
  if (ret >= 0)
    {
      if (ret >= size)
	__chk_fail ();
      memcpy (str, chk_sprintf_buf, ret + 1);
    }
  return ret;
}

int
__vsprintf_chk (char *str, int flag, __SIZE_TYPE__ size, const char *fmt,
		va_list ap)
{
  int ret;

  /* If size is -1 and flag 0, GCC should always optimize the call into
     vsprintf.  */
  if (size == (__SIZE_TYPE__) -1 && flag == 0)
    abort ();
  ++chk_calls;
#ifdef __OPTIMIZE__
  if (vsprintf_disallowed && inside_main)
    abort();
#endif
  ret = vsprintf (chk_sprintf_buf, fmt, ap);
  if (ret >= 0)
    {
      if (ret >= size)
	__chk_fail ();
      memcpy (str, chk_sprintf_buf, ret + 1);
    }
  return ret;
}

int
__snprintf_chk (char *str, __SIZE_TYPE__ len, int flag, __SIZE_TYPE__ size,
		const char *fmt, ...)
{
  int ret;
  va_list ap;

  /* If size is -1 and flag 0, GCC should always optimize the call into
     snprintf.  */
  if (size == (__SIZE_TYPE__) -1 && flag == 0)
    abort ();
  ++chk_calls;
  if (size < len)
    __chk_fail ();
#ifdef __OPTIMIZE__
  if (snprintf_disallowed && inside_main)
    abort();
#endif
  va_start (ap, fmt);
  ret = vsprintf (chk_sprintf_buf, fmt, ap);
  va_end (ap);
  if (ret >= 0)
    {
      if (ret < len)
	memcpy (str, chk_sprintf_buf, ret + 1);
      else
	{
	  memcpy (str, chk_sprintf_buf, len - 1);
	  str[len - 1] = '\0';
	}
    }
  return ret;
}

int
__vsnprintf_chk (char *str, __SIZE_TYPE__ len, int flag, __SIZE_TYPE__ size,
		 const char *fmt, va_list ap)
{
  int ret;

  /* If size is -1 and flag 0, GCC should always optimize the call into
     vsnprintf.  */
  if (size == (__SIZE_TYPE__) -1 && flag == 0)
    abort ();
  ++chk_calls;
  if (size < len)
    __chk_fail ();
#ifdef __OPTIMIZE__
  if (vsnprintf_disallowed && inside_main)
    abort();
#endif
  ret = vsprintf (chk_sprintf_buf, fmt, ap);
  if (ret >= 0)
    {
      if (ret < len)
	memcpy (str, chk_sprintf_buf, ret + 1);
      else
	{
	  memcpy (str, chk_sprintf_buf, len - 1);
	  str[len - 1] = '\0';
	}
    }
  return ret;
}

int
snprintf (char *str, __SIZE_TYPE__ len, const char *fmt, ...)
{
  int ret;
  va_list ap;

#ifdef __OPTIMIZE__
  if (snprintf_disallowed && inside_main)
    abort();
#endif
  va_start (ap, fmt);
  ret = vsprintf (chk_sprintf_buf, fmt, ap);
  va_end (ap);
  if (ret >= 0)
    {
      if (ret < len)
	memcpy (str, chk_sprintf_buf, ret + 1);
      else if (len)
	{
	  memcpy (str, chk_sprintf_buf, len - 1);
	  str[len - 1] = '\0';
	}
    }
  return ret;
}

/* uClibc's vsprintf calls vsnprintf.  */
#ifndef __UCLIBC__
int
vsnprintf (char *str, __SIZE_TYPE__ len, const char *fmt, va_list ap)
{
  int ret;

#ifdef __OPTIMIZE__
  if (vsnprintf_disallowed && inside_main)
    abort();
#endif
  ret = vsprintf (chk_sprintf_buf, fmt, ap);
  if (ret >= 0)
    {
      if (ret < len)
	memcpy (str, chk_sprintf_buf, ret + 1);
      else if (len)
	{
	  memcpy (str, chk_sprintf_buf, len - 1);
	  str[len - 1] = '\0';
	}
    }
  return ret;
}
#endif

#if defined(__powerpc__) && defined(__LONG_DOUBLE_IEEE128__)
__typeof (__sprintf_chk) __sprintf_chkieee128
  __attribute__((alias ("__sprintf_chk")));
__typeof (__vsprintf_chk) __vsprintf_chkieee128
  __attribute__((alias ("__vsprintf_chk")));
__typeof (__snprintf_chk) __snprintf_chkieee128
  __attribute__((alias ("__snprintf_chk")));
__typeof (__vsnprintf_chk) __vsnprintf_chkieee128
  __attribute__((alias ("__vsnprintf_chk")));
#endif
