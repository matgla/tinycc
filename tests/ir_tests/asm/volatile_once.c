/* A volatile access must happen exactly as many times as the SOURCE says --
 * too many is as wrong as too few.  Each function is named
 * <k><N>_<loads>_<stores> with the count one execution of the source body
 * performs (test_volatile_access_not_duplicated).
 *
 * `x` cases are compiled as written, so the object must hold exactly that
 * count.  `r` cases are self-recursive: at -O1/-O2 the frontend expands one
 * level of the recursion into the body, so the object holds TWO copies of the
 * count while each call now covers two levels of the recursion -- the product
 * is unchanged.  The test multiplies accordingly.
 *
 * The historical bug this file was written for looked the same by instruction
 * count but not by control flow: the expansion aborted at the depth cap after
 * emitting `int t = g;`, so the leftover load ran unconditionally while the
 * call still covered a single level.  What separates the two is where the
 * second copy sits relative to the guard, which only a dynamic count can see:
 * tests/ir_tests/461_self_inline_side_effects.c pins that. */

volatile int g;
struct S { volatile int a; int b; };
struct S gs;
extern void ext(void);

int  x01_1_0(void) { int t = g; (void)t; return 3; }
int  x02_1_0(void) { int t = g; (void)t; ext(); return 3; }
void r03_1_0(void) { int t = g; (void)t; r03_1_0(); }
void r04_1_0(void) { int t = g; r04_1_0(); }
void r05_1_0(void) { g; r05_1_0(); }
void r06_1_0(int n) { int t = g; (void)t; if (n) r06_1_0(n - 1); }
int  r07_1_0(void) { int t = g; (void)t; return r07_1_0(); }
void r08_1_0(void) { int t = gs.a; (void)t; r08_1_0(); }
void r09_0_1(void) { g = 1; r09_0_1(); }
