/*
 *  TCC IR - guest output the test harness must not lose
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Driven by test_qemu_output_capture.py, not by TEST_FILES.  SubprocessSUT read
 * QEMU through a pipe: -nographic makes it O_NONBLOCK, so a full pipe dropped
 * whole semihosting writes and the guest still exited 0. */

extern int printf(const char *, ...);

int main(void)
{
#if defined(HANG)
  printf("hanging\n");
  for (;;)
    ;
#elif defined(SPAM)
  for (;;)
    printf("spam spam spam spam spam spam spam spam\n");
#else
  for (int i = 0; i < 10000; i++)
    printf("line %05d abcdefghijklmnopqrstuvwxyz012\n", i);
  printf("done");
  return 7;
#endif
}
