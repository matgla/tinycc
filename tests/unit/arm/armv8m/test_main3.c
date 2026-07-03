/*
 *  test_main3.c - entry point for tccgen.c unit-test binary
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(tccgen);

int main(void)
{
  UT_RUN_SUITE(tccgen);
  UT_REPORT_AND_EXIT();
}
