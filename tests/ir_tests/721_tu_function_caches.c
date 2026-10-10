/* First file of two compiled by one tcc command.  The -O2 function caches
 * (purity, constant result, switch snapshot) are keyed by token number, and
 * token numbers start over with the second file: there g0..g3 got the
 * numbers of f0..f3 here, so main's calls to them were folded to f0..f3's
 * constants.  f0..f3 come first so that they take the first numbers. */
int f0(void) { return 11; }
int f1(void) { return 12; }
int f2(void) { return 13; }
int f3(void) { return 14; }

int g0(void) { return 20; }
int g1(void) { return 21; }
int g2(void) { return 22; }
int g3(void) { return 23; }
