/* Volatile accesses must survive every CSE / DSE / store-forwarding pass.
 *
 * Each function's name encodes what the codegen test asserts:
 *   v<N>_<loads>_<stores>  — a volatile case: at least that many real ldr/str
 *   c<N>_<loads>_<stores>  — the same shape WITHOUT volatile, as a control that
 *                            the optimization being blocked above still fires
 * (literal-pool `ldr rX,[pc,...]` loads are not counted.)
 */

volatile int g;
volatile int ga[4];
struct S
{
  int a;
  volatile int b;
};
struct P
{
  int a;
  int b;
};
volatile struct P vs;
struct S gs;

/* --- pointer deref ------------------------------------------------------- */
int v01_2_0(volatile int *p) { return *p + *p; }
int v02_2_0(volatile int *p) { return p[1] + p[1]; }
int v03_2_0(volatile int *p, int i) { return p[i] + p[i]; }
int v04_2_0(struct S *s) { return s->b + s->b; }
int v05_2_0(volatile int *p)
{
  int a = *p;
  int b = *p;
  return a + b;
}

/* --- pointer stores ------------------------------------------------------ */
void v06_0_2(volatile int *p) { *p = 1; *p = 2; }
void v07_0_2(volatile int *p) { p[1] = 1; p[1] = 2; }
void v08_0_2(struct S *s) { s->b = 1; s->b = 2; }

/* --- store then load ----------------------------------------------------- */
int v09_1_1(volatile int *p) { *p = 3; return *p; }
int v10_1_1(volatile int *p) { p[1] = 3; return p[1]; }
int v11_1_1(struct S *s) { s->b = 3; return s->b; }

/* --- globals ------------------------------------------------------------- */
void v12_0_2(void) { g = 1; g = 2; }
void v13_0_2(void) { ga[1] = 1; ga[1] = 2; }
void v14_0_2(void) { gs.b = 1; gs.b = 2; }
void v15_0_2(void) { vs.a = 1; vs.a = 2; }
int v16_2_0(void) { return g + g; }
int v17_2_0(void) { return ga[1] + ga[1]; }
int v18_2_0(void) { return gs.b + gs.b; }
int v19_2_0(void) { return vs.a + vs.a; }
int v20_1_1(void) { g = 5; return g; }
int v21_1_1(void) { ga[2] = 5; return ga[2]; }

/* --- locals -------------------------------------------------------------- */
int v22_2_1(void)
{
  volatile int x = 5;
  return x + x;
}
int v23_1_1(void)
{
  volatile int x;
  x = 7;
  return x;
}
int v24_0_2(void)
{
  volatile int x;
  x = 1;
  x = 2;
  return 0;
}

/* --- casts --------------------------------------------------------------- */
int v25_2_0(int *p) { return *(volatile int *)p + *(volatile int *)p; }

/* --- the value is discarded: the read still happens ---------------------- */
void v26_1_0(volatile int *p) { (void)*p; }
void v27_1_0(void) { (void)g; }
void v30_1_0(volatile int *p) { *p; }
void v31_2_0(volatile int *p) { (void)(*p, *p); }

/* --- loops: the access stays inside the loop ----------------------------- */
int v28_1_0(volatile int *p, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += p[1];
  return s;
}
int v29_1_0(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += g;
  return s;
}

/* --- absolute-address MMIO registers ------------------------------------- */
#define REG (*(volatile unsigned int *)0x40000000)
#define REG2 (*(volatile unsigned int *)0x40000004)
unsigned int v40_1_0(void) { return REG; }
void v41_0_2(void) { REG = 1; REG = 2; }
unsigned int v42_2_0(void) { return REG + REG; }
unsigned int v43_2_0(void)
{
  while (!(REG & 1))
    ;
  return REG2;
}
#define REG64 (*(volatile unsigned long long *)0x40000010)
unsigned long long v44_2_0(void) { return REG64; }
void v45_0_2(unsigned long long x) { REG64 = x; }

/* --- loop hoisting, GVN across a branch, byte merging, if-conversion ----- */
struct B
{
  volatile unsigned char a, b, c, d;
};
volatile struct B vb;

int sink(int);

int w01_1_0(struct S *s, int n)
{
  int t = 0;
  for (int i = 0; i < n; i++)
    t += s->b;
  return t;
}
int w02_1_0(int n)
{
  int t = 0;
  for (int i = 0; i < n; i++)
    t += ga[1];
  return t;
}
int w03_0_1(volatile int *p, int n)
{
  for (int i = 0; i < n; i++)
    *p = 1;
  return 0;
}
int w04_2_0(volatile int *p, int c)
{
  int a = p[1];
  if (c)
    sink(a);
  return p[1];
}
/* four byte writes to a register block are four bus writes, not one word */
void w05_0_4(void) { vb.a = 1; vb.b = 2; vb.c = 3; vb.d = 4; }
void w06_0_4(struct B *b) { b->a = 1; b->b = 2; b->c = 3; b->d = 4; }
/* if-conversion must not speculate a volatile access */
int w07_1_0(volatile int *p, int c)
{
  int t = 0;
  if (c)
    t = *p;
  return t;
}
void w08_0_1(volatile int *p, int c)
{
  if (c)
    *p = 1;
}
/* read-modify-write keeps both halves; storing the same value twice stores twice */
void w09_1_1(volatile int *p) { *p = *p + 1; }
void w10_1_1(void) { g = g + 1; }
void w11_0_2(volatile int *p) { *p = 1; *p = 1; }

/* --- in a loop: the trip count must not collapse a mandated access ---------
 * A dead destination made these look pure, so dead_var_store / dse dropped the
 * load outright; ssa:dead_loop would then have run the body once.  The loop has
 * to survive with its access, so >=1 real ldr and a back edge. */
static int dead_sink;
int v46_1_0(int n) { int r = 0; for (int i = 0; i < n; i++) { int t = g; (void)t; r = 3; } return r; }
int v47_1_0(int n) { int r = 0; for (int i = 0; i < n; i++) { dead_sink = g; r = 3; } return r; } /* the store to a never-read static is genuinely dead; only the load is mandated */
int v48_1_0(int n) { volatile int v = 0; int r = 0;
                     for (int i = 0; i < n; i++) { int t = v; (void)t; r = 3; } return r; }
/* control: identical shape, non-volatile source -- the loop SHOULD collapse */
static int nv;
int c08_0_0(int n) { int r = 0; for (int i = 0; i < n; i++) { int t = nv; (void)t; r = 3; } return r; }

/* --- volatility a symbol-level `sym->type.t & VT_VOLATILE` test cannot see ---
 * When only a MEMBER is volatile the symbol's own type is not, so every pass
 * gating on the symbol alone folded these: global_init_prop replaced the load
 * with the zero initialiser, and invariant_global_load_hoist CSEd two mandated
 * reads into one. */
struct VS { volatile int a; int b; };
struct VN { int pad; struct VS s; };
struct VA { volatile int v[4]; };
struct VB { volatile unsigned f1 : 4; volatile unsigned f2 : 4; };
static struct VS mgs;
static struct VN mgn;
static struct VA mga;
static struct VB mgb;
static int mdead;

int v49_2_0(void)  { int x = mgs.a; int y = mgs.a; return x + y; }
int v50_1_0(int n) { int r = 0; for (int i = 0; i < n; i++) { int t = mgs.a;   (void)t; r = 3; } return r; }
int v51_1_0(int n) { int r = 0; for (int i = 0; i < n; i++) { int t = mgn.s.a; (void)t; r = 3; } return r; }
int v52_1_0(int n) { int r = 0; for (int i = 0; i < n; i++) { int t = mga.v[2];(void)t; r = 3; } return r; }
int v53_1_0(int n) { int r = 0; for (int i = 0; i < n; i++) { mdead = mgs.a;   r = 3; } return r; }
int v54_1_0(void)  { unsigned t = mgb.f1; (void)t; return 3; }
/* control: the non-volatile sibling of the same struct still collapses */
int c09_0_0(int n) { int r = 0; for (int i = 0; i < n; i++) { int t = mgs.b; (void)t; r = 3; } return r; }

/* --- the access survives every IR pass and dies in a post-allocation DCE ----
 * Both of these have a live volatile access and a DEAD destination.  The flags
 * of a comparison nobody reads, and the result of a load nobody reads, are not
 * reasons to skip reaching the operand.  orphan_cmp_elim dropped the first;
 * noreturn_collapse collapsed the second to a bare self-jump. */
int  v55_1_0(void) { int t = (mgs.a == 3); (void)t; return 7; }
void v56_1_0(void) { for (;;) { int t = mgs.a; (void)t; } }

/* --- the post-allocation DCE group ------------------------------------------
 * run_post_alloc_passes() elides a whole function body when what remains is
 * undefined, unreachable or useless.  Each of those passes scanned for a
 * volatile SYMBOL, so a volatile member slipped past and the mandated access
 * went with the body.  gcc keeps the access in all three. */
int  v57_1_0(void) { int t = mgs.a; (void)t; *(volatile int *)0 = 1; return 3; }
void v58_1_0(void) { int t = mgs.a; (void)t; v58_1_0(); }
void v59_1_0(void) { int acc = 0; for (;;) { acc += mgs.a; } }

/* --- deciding a comparison does not remove the reads it is made of ----------
 * `x == x` is true whatever x is, but both reads still have to happen.  Two
 * passes proved the operands equal without asking: cmp_expr_fold reached it
 * through its generic expression-equality helper, ahead of its own volatile
 * check, and ssa:branch through a symref-identity test that sees only the
 * symbol's type.  gcc reads twice and compares. */
int v60_2_0(void) { return g == g; }
int v61_2_0(void) { return mgs.a == mgs.a; }
int v62_2_0(void) { return mgs.a - mgs.a; }
/* a volatile member read in a loop must not be hoisted out of it */
int v63_1_0(int n) { int s = 0; for (int i = 0; i < n; i++) { s += mgs.a; mdead = i; } return s; }

/* --- a struct copy is lowered to word loads typed by the WORD ---------------
 * ...not by the member each lands on, so member-level volatility was lost at
 * the one point that records it (svalue_to_iroperand).  With the access marked
 * provably-non-volatile, global_init_prop replaced the load of a never-written
 * static with its zero initialiser: `struct { volatile int a; } t = g;` then
 * returned a constant instead of reading the register. */
struct VW { volatile int a; };
static struct VW mgw;
int v64_1_0(void) { struct VW t = mgw; return t.a; }
int v65_2_0(void) { struct VW x = mgw; struct VW y = mgw; return x.a + y.a; }
int v66_1_0(void) { struct VW t = mgw; mdead = t.a; return 0; }

/* --- controls: the same shapes without volatile -------------------------- */
int c01_1_0(int *p) { return *p + *p; }
int c02_1_0(int *p) { return p[1] + p[1]; }
int c03_1_0(struct P *s) { return s->b + s->b; }
void c04_0_1(int *p) { *p = 1; *p = 2; }
int c05_0_1(int *p) { *p = 3; return *p; }
int c06_0_0(struct P *s, int n)
{
  int t = 0;
  for (int i = 0; i < n; i++)
    t += s->b;
  return t;
}
struct N
{
  unsigned char a, b, c, d;
};
void c07_0_1(struct N *n) { n->a = 1; n->b = 2; n->c = 3; n->d = 4; }
