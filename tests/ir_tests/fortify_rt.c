/* A _FORTIFY_SOURCE runtime with glibc's names (yasos libc had it until tcc
 * checked the calls itself), for the QEMU runs of test_fortify.py, which
 * link newlib.  Never fortified
 * itself (libc's Makefile says -U_FORTIFY_SOURCE): it defines the functions
 * the fortified headers turn into macros.  The str* checks tcc calls are
 * libtcc1's __tcc_*_chk, which end in this __chk_fail too. */
#undef _FORTIFY_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((noreturn)) void __chk_fail(void)
{
  static const char msg[] = "*** buffer overflow detected ***: terminated\n";
  fflush(stdout);
  write(2, msg, sizeof msg - 1);
  abort();
}

void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen)
{
  if (n > dstlen)
    __chk_fail();
  return memcpy(dst, src, n);
}

void *__memmove_chk(void *dst, const void *src, size_t n, size_t dstlen)
{
  if (n > dstlen)
    __chk_fail();
  return memmove(dst, src, n);
}

void *__mempcpy_chk(void *dst, const void *src, size_t n, size_t dstlen)
{
  if (n > dstlen)
    __chk_fail();
  return (char *)memcpy(dst, src, n) + n;
}

void *__memset_chk(void *s, int v, size_t n, size_t dstlen)
{
  if (n > dstlen)
    __chk_fail();
  return memset(s, v, n);
}

int __vsnprintf_chk(char *dst, size_t sz, int flag, size_t dstlen, const char *fmt, va_list ap)
{
  (void)flag;
  if (sz > dstlen)
    __chk_fail();
  return vsnprintf(dst, sz, fmt, ap);
}

int __vsprintf_chk(char *dst, int flag, size_t dstlen, const char *fmt, va_list ap)
{
  int ret;
  (void)flag;
  if (dstlen == (size_t)-1)
    return vsprintf(dst, fmt, ap);
  if (dstlen == 0)
    __chk_fail();
  ret = vsnprintf(dst, dstlen, fmt, ap);
  if (ret >= 0 && (size_t)ret >= dstlen)
    __chk_fail();
  return ret;
}

int __snprintf_chk(char *dst, size_t sz, int flag, size_t dstlen, const char *fmt, ...)
{
  va_list ap;
  int ret;
  va_start(ap, fmt);
  ret = __vsnprintf_chk(dst, sz, flag, dstlen, fmt, ap);
  va_end(ap);
  return ret;
}

int __sprintf_chk(char *dst, int flag, size_t dstlen, const char *fmt, ...)
{
  va_list ap;
  int ret;
  va_start(ap, fmt);
  ret = __vsprintf_chk(dst, flag, dstlen, fmt, ap);
  va_end(ap);
  return ret;
}
