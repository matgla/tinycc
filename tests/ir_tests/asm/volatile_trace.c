/* Volatile accesses: exact width, count and order (test_volatile_trace.py).
 *
 * volatile_access.c asserts MINIMUM counts, which cannot see an access that is
 * duplicated, widened (four strb -> one str), fused (two str -> one strd), or
 * reordered.  Here every case carries the exact trace of real memory accesses
 * it must compile to, at every -O level, in the comment right above it:
 *
 *   L<w> / S<w>   a load / store of w bytes (1, 2, 4, 8 = LDRD/STRD, m<n> = LDM/STM)
 *   @off          ...at that immediate offset (checked above -O0 only, where
 *                 the address is not yet folded into the instruction)
 *   C:<sym>       a call
 *
 * Literal-pool loads are not accesses.  Stack (sp-based) accesses are left out
 * unless the case says `stack`, which is how volatile locals are checked.
 *
 * Clauses, separated by `;`:
 *   expect: T...      the trace at every level; `|` separates acceptable
 *                     alternatives
 *   O0: / O1: / O2: / Os: T...   a per-level override of `expect`
 *   each: T...        for loops, whose static count depends on unrolling:
 *                     every access is one of these, and each appears
 *   unordered         compare as a multiset (unsequenced operands)
 *   loadwords>=: N    an aggregate copy: at least N words loaded
 *   stack             include stack accesses (and ignore the sp/reg base split)
 *   bug(LEVELS): slug a known open bug, docs/bugs/<slug>.md; strict xfail at
 *                     those levels, so a fix must also delete the clause
 *
 * The expectations were taken from arm-none-eabi-gcc -O2 (cortex-m33) and
 * reviewed case by case; where tcc may legitimately differ (a jump-table read,
 * a helper not inlined at -O0, a loop not unrolled) the case says so. */

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long uptr;

extern void ext(void);
extern int sink(int);

/* ======================================================================
 * Access width: a volatile access is exactly as wide as its type.
 * ==================================================================== */

/*@ expect: L1 */
u32 width_u8_load(volatile u8 *p) { return *p; }
/*@ expect: L2 */
u32 width_u16_load(volatile u16 *p) { return *p; }
/*@ expect: L1 */
int width_s8_load(volatile s8 *p) { return *p; }
/*@ expect: S1 */
void width_u8_store(volatile u8 *p) { *p = 1; }
/*@ expect: S2 */
void width_u16_store(volatile u16 *p) { *p = 1; }
/* byte reads must not be merged into one word read */
/*@ expect: L1 L1 L1 L1 */
u32 width_byte_loads_not_merged(volatile u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
/*@ expect: L2 L2 */
u32 width_half_loads_not_merged(volatile u16 *p) { return p[0] | (u32)p[1] << 16; }
/*@ expect: L4 L4 ; unordered */
u32 width_word_loads_not_paired(volatile u32 *p) { return p[0] + p[1]; }
/*@ expect: S1 S1 S1 S1 */
void width_byte_stores_not_merged(volatile u8 *d) { d[0] = 1; d[1] = 2; d[2] = 3; d[3] = 4; }
/*@ expect: S2 S2 */
void width_half_stores_not_merged(volatile u16 *p) { p[0] = 1; p[1] = 2; }
/*@ expect: L8 */
u64 width_u64_load(volatile u64 *p) { return *p; }
/*@ expect: S8 */
void width_u64_store(volatile u64 *p, u64 v) { *p = v; }
/*@ expect: L4 */
float width_float_load(volatile float *p) { return *p; }
/*@ expect: L8 */
double width_double_load(volatile double *p) { return *p; }
/*@ expect: S8 */
void width_double_store(volatile double *p, double d) { *p = d; }

/* A word register whose use needs only some bits is still read as a word:
 * narrowing `*p >> 24` to `ldrb [p,#3]` changes the bus access. */
/*@ expect: L4 */
u32 narrow_mask_low_byte(volatile u32 *p) { return *p & 0xff; }
/*@ expect: L4 */
u32 narrow_top_byte(volatile u32 *p) { return *p >> 24; }
/*@ expect: L4 */
u8 narrow_cast_u8(volatile u32 *p) { return (u8)*p; }
/*@ expect: L4 */
int narrow_test_bit16(volatile u32 *p) { return (*p & 0x10000) != 0; }
/*@ expect: L4 S4 */
void narrow_rmw_low_byte(volatile u32 *p) { *p = (*p & ~0xffu) | 5; }
/*@ expect: L4 */
u32 narrow_middle_byte(volatile u32 *p) { return (*p >> 8) & 0xff; }
/*@ expect: L4 */
u16 narrow_high_half(volatile u32 *p) { return (u16)(*p >> 16); }

/* Volatile bitfields are accessed with their container's width (AAPCS). */
struct BF { volatile u32 f1 : 4; volatile u32 f2 : 4; volatile u32 f3 : 24; };
struct BF8 { volatile u8 f1 : 4; volatile u8 f2 : 4; };
struct BFR { u32 f1 : 4; u32 f2 : 4; u32 rest : 24; };
/*@ expect: L4 */
u32 bitfield_read(struct BF *b) { return b->f2; }
/*@ expect: L1 */
u32 bitfield_read_u8_container(struct BF8 *b) { return b->f2; }
/*@ expect: L4 */
u32 bitfield_read_volatile_struct(volatile struct BFR *b) { return b->f2; }
/*@ expect: L4 L4 S4 */
void bitfield_postinc(struct BF *b) { b->f2++; }
/* An assignment to a bitfield is one read-modify-write; its value is the
 * value stored, not a fresh read of the field. */
/*@ expect: L4 S4 */
void bitfield_write(struct BF *b) { b->f2 = 3; }
/*@ expect: L4 S4 */
void bitfield_write_wide(struct BF *b) { b->f3 = 0x123; }
/*@ expect: L4 S4 */
void bitfield_write_volatile_struct(volatile struct BFR *b) { b->f1 = 1; }
/*@ expect: L4 S4 */
u32 bitfield_write_value_used(struct BF *b) { return b->f2 = 3; }
/*@ expect: L4 L4 S4 */
void bitfield_compound(struct BF *b) { b->f2 += 1; }
/*@ expect: L4 S4 L4 S4 */
void bitfield_two_writes(struct BF *b) { b->f1 = 1; b->f2 = 2; }

/* ======================================================================
 * Store pairing: two volatile word stores are two STRs.  STRD/STM are
 * multi-access instructions an exception may abandon and restart, which
 * repeats a device write -- gcc never pairs volatile scalars.
 * ==================================================================== */

/*@ expect: S4@0 S4@4 */
void pair_two_stores(volatile u32 *p) { p[0] = 1; p[1] = 2; }
/*@ expect: S4@0 S4@4 */
void pair_two_reg_stores(volatile int *p, int x, int y) { p[0] = x; p[1] = y; }
/*@ expect: S4@4 S4@8 */
void pair_offset_four(volatile int *p) { p[1] = 1; p[2] = 2; }
/*@ expect: S4@0 S4@4 S4@8 S4@12 */
void pair_four_zero_stores(volatile u32 *p) { p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0; }
/*@ expect: S4@0 S4@4 S4@8 */
void pair_three_stores_in_order(volatile u32 *p) { p[0] = 1; p[1] = 2; p[2] = 3; }
/*@ expect: S4@8 S4@0 S4@4 */
void pair_three_stores_out_of_order(volatile u32 *p) { p[2] = 1; p[0] = 2; p[1] = 3; }
struct V2 { volatile int a, b; };
/*@ expect: S4@0 S4@4 */
void pair_volatile_members(struct V2 *s) { s->a = 1; s->b = 2; }
struct regs { u32 a, b, c; u32 ch[4]; };
/*@ expect: S4@0 S4@4 L4@8 */
u32 pair_volatile_struct_ptr(volatile struct regs *r) { r->a = 1; r->b = 2; return r->c; }
/*@ each: S4 */
void pair_unrolled_word_loop(volatile u32 *d) { for (int i = 0; i < 4; i++) d[i] = 0; }
/* globals and absolute addresses already stay separate */
struct P2 { int a, b; };
volatile struct P2 vs2;
struct V2 mv2;
/*@ expect: S4 S4 */
void pair_volatile_global_struct(void) { vs2.a = 1; vs2.b = 2; }
/*@ expect: S4 S4 */
void pair_global_volatile_members(void) { mv2.a = 1; mv2.b = 2; }
#define REG0 (*(volatile u32 *)0x40000000)
#define REG1 (*(volatile u32 *)0x40000004)
/*@ expect: S4 S4 */
void pair_mmio_registers(void) { REG0 = 1; REG1 = 2; }
/*@ expect: L4 L4 */
int pair_mmio_register_reads(void) { return REG0 + REG1; }
/*@ expect: S8 S8 */
void pair_u64_stores(volatile u64 *p) { p[0] = 1; p[1] = 2; }

/* ======================================================================
 * Byte stores widened: a loop of volatile byte stores, once unrolled, must
 * stay byte stores.
 * ==================================================================== */

/*@ each: S1 */
void widen_zero_bytes_4(volatile u8 *d) { for (int i = 0; i < 4; i++) d[i] = 0; }
/*@ each: S1 */
void widen_zero_bytes_8(volatile u8 *d) { for (int i = 0; i < 8; i++) d[i] = 0; }
/*@ each: S1 */
void widen_zero_bytes_16(volatile u8 *d) { for (int i = 0; i < 16; i++) d[i] = 0; }
/*@ each: S1 */
void widen_index_bytes(volatile u8 *d) { for (int i = 0; i < 4; i++) d[i] = i; }
volatile u8 gbuf[16];
/*@ each: S1 */
void widen_global_byte_array(void) { for (int i = 0; i < 16; i++) gbuf[i] = 0; }
/*@ each: S2 */
void widen_zero_halves(volatile u16 *d) { for (int i = 0; i < 4; i++) d[i] = 0; }
/*@ each: L1 */
u32 widen_byte_load_loop(volatile u8 *d) { u32 s = 0; for (int i = 0; i < 4; i++) s |= d[i] << (8 * i); return s; }
/*@ expect: S1 S1 S1 S1 */
void widen_zero_bytes_straight(volatile u8 *d) { d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 0; }

/* ======================================================================
 * Exact count: compound assignment, increments, the value of an assignment.
 * ==================================================================== */

/*@ expect: L4 S4 */
void count_or_assign(volatile u32 *p) { *p |= 1; }
/*@ expect: L4 S4 */
void count_preinc_stmt(volatile u32 *p) { ++*p; }
/*@ expect: L4 S4 */
u32 count_postinc_value(volatile u32 *p) { return (*p)++; }
/*@ expect: L4 S4 */
u32 count_preinc_value(volatile u32 *p) { return ++*p; }
/* the value of an assignment is the value stored, not a re-read */
/*@ expect: S4 */
u32 count_assign_value(volatile u32 *p) { return *p = 5; }
/*@ expect: L4 S4 */
void count_copy(volatile u32 *p, volatile u32 *q) { *q = *p; }
/*@ expect: L4 S4 */
void count_self_assign(volatile u32 *p) { *p = *p; }
/*@ expect: L4 L4 S4 */
void count_add_self(volatile u32 *p) { *p += *p; }
/*@ expect: L4 S4 L4 S4 */
void count_xor_twice(volatile u32 *p) { *p ^= 1; *p ^= 1; }
volatile u32 g;
volatile u8 gb;
volatile u16 gh;
volatile u64 gq;
/*@ expect: L4 S4 L4 S4 */
void count_global_set_clear(void) { g |= 1; g &= ~1u; }
/*@ expect: L1 S1 L2 S2 */
void count_global_narrow_rmw(void) { gb = gb + 1; gh |= 2; }
/* a 64-bit volatile read is two word accesses either way */
/*@ expect: L8 S8 | L4 L4 S8 */
void count_global_u64_inc(void) { gq++; }
/*@ expect: S4 S4 S4 */
void count_three_equal_stores(volatile int *p) { *p = 0; *p = 0; *p = 0; }
/*@ expect: S4 S4 */
void count_same_value_twice(volatile u32 *p, u32 v) { p[0] = v; p[0] = v + 0; }
/*@ expect: L4 L4 */
int count_dead_first_read(volatile int *p) { int x = *p; x = *p; return x; }
/*@ expect: L4 L4 L4 */
u32 count_three_reads(volatile u32 *p) { u32 a = *p; return a + *p + *p; }
/*@ expect: L4 */
u32 count_one_read_reused(volatile u32 *p) { u32 x = *p; return x * 2 + x; }
/*@ expect: L4 C:sink L4 C:sink */
void count_read_per_call(volatile int *p) { sink(*p); sink(*p); }

/* ======================================================================
 * A read is not dropped because the result is known without it.
 * ==================================================================== */

/*@ expect: L4 L4 */
int known_or_chain(volatile int *p) { return *p == 1 || *p == 2; }
/*@ expect: L4 L4 */
int known_contradiction(volatile int *p) { return *p == 1 && *p == 2; }
/*@ expect: L4 L4 */
int known_xor_self(volatile int *p) { return *p ^ *p; }
/*@ expect: L4 L4 */
int known_tautology(volatile int *p) { return *p >= 0 || *p < 0; }
/*@ expect: L4 L4 */
int known_two_reads_equal(volatile int *p) { int a = *p; int b = *p; return a == b; }
/*@ expect: L4 */
int known_ternary_same_arms(volatile int *p) { return *p ? 1 : 1; }
/*@ expect: L1 */
int known_u8_out_of_range(volatile u8 *p) { return *p > 300; }
/*@ expect: L1 */
int known_u8_nonneg(volatile u8 *p) { return *p >= 0; }
/*@ expect: L4 */
int known_and_false(volatile int *p) { return *p && 0; }
/*@ expect: L4 */
int known_or_true(volatile int *p) { return *p || 1; }
/* `x & 0`, `x * 0`, `x | -1`, `x % 1` are constants, but x is still read */
/*@ expect: L4 */
int known_and_zero(volatile int *p) { return *p & 0; }
/*@ expect: L4 */
int known_mul_zero(volatile int *p) { return *p * 0; }
/*@ expect: L4 */
int known_zero_mul(volatile int *p) { return 0 * *p; }
/*@ expect: L4 */
int known_global_mul_zero(void) { return g * 0; }
/*@ expect: L4 */
u32 known_mmio_and_zero(void) { return REG0 & 0; }
/*@ expect: L4 */
int known_mod_one(volatile int *p) { return *p % 1; }
/*@ expect: L4 */
int known_or_all_ones(volatile int *p) { return *p | -1; }
/*@ expect: L4 */
int known_mul_zero_into_local(volatile int *p) { int x = *p * 0; return x; }
/*@ expect: L4 */
int known_masked_compare(volatile int *p) { return (*p & 0) == 0; }
/*@ expect: L4 */
int known_zero_shifted(volatile int *p) { return 0 << *p; }
/*@ expect: L4 */
int known_minus_one_shifted(volatile int *p) { return -1 >> *p; }
/* ...while these genuinely do not read */
/*@ expect: */
int known_short_circuit_skips(volatile int *p) { return 0 && *p; }
/*@ expect: */
int known_sizeof_skips(volatile int *p) { return sizeof(*p); }
/*@ expect: */
volatile int *known_address_of_skips(volatile int *p) { return &*p; }

/* ssa:gvn: two reads of a volatile object through a pointer are not one value
 * (docs/bugs/ssa-gvn-cse-merges-volatile-pointer-reads, fixed) */
struct vs { int x; };
/*@ expect: L4 L4 */
int gvn_alu_two_reads(volatile int *p, int k) { int a = *p + k; int b = *p + k; return a ^ b; }
/*@ expect: L4 L4 */
int gvn_cmp_two_reads(volatile int *p) { return (*p < 5) + (*p < 5); }
/*@ expect: L4 L4 */
int gvn_field_two_reads(volatile struct vs *s, int k) { int a = s->x & k; int b = s->x & k; return a - b; }

/* ======================================================================
 * A discarded value: the read still happens.
 * ==================================================================== */

/*@ expect: L4 */
void discard_cast_void(volatile int *p) { (int)*p; }
/*@ expect: L4 */
int discard_comma(volatile int *p) { return (*p, 5); }
/*@ expect: L4 */
void discard_compare(volatile int *p) { (void)(*p == 1); }
/*@ expect: L4 */
void discard_not(volatile int *p) { (void)!*p; }
/*@ expect: L4 */
void discard_narrow_cast(volatile int *p) { (void)(u8)*p; }
/*@ expect: L4 */
void discard_into_dead_local(volatile int *p) { int x = *p + 1; (void)x; }
/*@ expect: L4 */
int discard_dead_local(volatile int *p) { int x = *p; (void)x; return 3; }
/*@ expect: L4 */
void discard_global_add(void) { (void)(g + 1); }
/* an arithmetic op on the read, its result unused */
/*@ expect: L4 */
void discard_add_stmt(volatile int *p) { *p + 1; }
/*@ expect: L4 */
void discard_add_void(volatile int *p) { (void)(*p + 1); }
/*@ expect: L4 */
void discard_negate(volatile int *p) { (void)-*p; }
/*@ expect: L4 */
void discard_shift(volatile int *p) { (void)(*p << 2); }
/*@ expect: L4 L4 */
void discard_sum_of_two(volatile int *p) { (void)(*p + *p); }

/* ======================================================================
 * Order: volatile accesses happen in source order.
 * ==================================================================== */

/*@ expect: L4@4 L4@0 */
int order_two_loads(volatile int *p) { int a = p[1]; int b = p[0]; return a - b; }
/*@ expect: S4@0 L4@4 */
int order_store_then_load(volatile int *p) { p[0] = 1; return p[1]; }
/*@ expect: L4@4 S4@0 */
int order_load_then_store(volatile int *p) { int x = p[1]; p[0] = 1; return x; }
/*@ expect: L4 L4 */
int order_two_pointers(volatile int *p, volatile int *q) { int a = *q; int b = *p; return a - b; }
/*@ expect: S1@3 S1@2 S1@1 S1@0 */
void order_bytes_descending(volatile u8 *p) { p[3] = 1; p[2] = 2; p[1] = 3; p[0] = 4; }
/*@ expect: L4@0 S4@4 L4@4 S4@0 */
void order_swap_through(volatile u32 *p) { p[1] = p[0]; p[0] = p[1]; }
/*@ expect: S4@4 C:ext S4@0 */
void order_around_call(volatile u32 *p) { p[1] = 1; ext(); p[0] = 2; }
/*@ expect: S4@0 L4@4 */
void order_poll_after_kick(volatile u32 *p) { do { p[0] = 1; } while (!(p[1] & 1)); }
/* `+` does not sequence its operands */
union U { volatile u32 w; volatile u8 b[4]; };
/*@ expect: L4 L1 ; unordered */
u32 order_unsequenced(union U *u) { return u->w + u->b[1]; }

/* ======================================================================
 * Loops.
 * ==================================================================== */

/*@ each: S4 */
void loop_store_each_iteration(volatile int *p) { for (int i = 0; i < 4; i++) *p = i; }
/*@ each: L4 */
int loop_load_each_iteration(volatile int *p) { int s = 0; for (int i = 0; i < 4; i++) s += *p; return s; }
/*@ each: S4 */
void loop_store_not_sunk(int n) { for (int i = 0; i < n; i++) g = i; }
/*@ each: L4 */
int loop_poll_with_timeout(volatile u32 *p, int n) { while (!(*p & 1) && --n) ; return n; }
/*@ each: L4 S4 */
void loop_copy_from_volatile(int *d, volatile int *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
/*@ each: S4 */
void loop_clear_volatile(volatile int *d, int n) { for (int i = 0; i < n; i++) d[i] = 0; }
/*@ each: L4 S4 */
void loop_copy_to_volatile(volatile int *d, const int *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
/*@ each: S4 */
void loop_toggle(volatile int *p) { for (int i = 0; i < 2; i++) { p[0] = 1; p[0] = 0; } }
/*@ each: L4 */
int loop_count_while_set(volatile int *p) { int n = 0; while (*p) n++; return n; }

/* ======================================================================
 * Inlined helpers: each call's access survives inlining (at -O0 the helper
 * stays a call).
 * ==================================================================== */

static inline u32 rd(volatile u32 *r) { return *r; }
static inline void wr(volatile u32 *r, u32 v) { *r = v; }
static inline void set_bits(volatile u32 *r, u32 m) { *(volatile u32 *)((uptr)r | 0x2000) = m; }
#define R ((volatile u32 *)0x40000000)
#define R2 ((volatile u32 *)0x40000004)
/*@ expect: L4 L4 ; O0: C:rd C:rd */
u32 inline_two_reads(void) { return rd(R) + rd(R); }
/*@ expect: S4 S4 ; O0: C:wr C:wr */
void inline_two_writes(void) { wr(R, 1); wr(R, 1); }
/*@ expect: L4 ; O0: C:rd */
void inline_poll(void) { while (!(rd(R) & 1)) ; }
/*@ expect: S4 S4 ; O0: C:set_bits C:set_bits */
void inline_alias_writes(void) { set_bits(R, 1); set_bits(R, 1); }
static void helper_once(volatile u32 *r) { *r = 1; *r = 1; (void)*r; }
/*@ expect: S4 S4 L4 ; O0: C:helper_once */
void inline_called_once(void) { helper_once(R2); }
static inline u32 rd_and_drop(volatile u32 *r) { (void)*r; return 7; }
/*@ expect: L4 ; O0: C:rd_and_drop */
u32 inline_discarded_read(void) { return rd_and_drop(R); }
static inline int rd_eq(volatile u32 *r, u32 v) { return *r == v; }
/*@ expect: L4 L4 ; O0: C:rd_eq C:rd_eq */
int inline_compare_chain(void) { return rd_eq(R, 1) || rd_eq(R, 2); }
struct hw { volatile u32 ctrl, status, data[4]; };
static inline void hw_wait(struct hw *h) { while (h->status & 1) ; }
/*@ expect: L4@4 S4@0 L4@4 L4@16 ; O0: C:hw_wait S4 C:hw_wait L4 */
u32 inline_wait_kick_wait(struct hw *h) { hw_wait(h); h->ctrl = 1; hw_wait(h); return h->data[2]; }

/* ======================================================================
 * Pointers and register blocks.
 * ==================================================================== */

int *volatile vp;
volatile int *volatile vvp;
/* the pointer itself is volatile: read it twice */
/*@ expect: L4 L4 L4 L4 */
int ptr_volatile_pointer(void) { return *vp + *vp; }
/*@ expect: L4 L4 L4 L4 */
int ptr_volatile_both(void) { return *vvp + *vvp; }
/*@ expect: L4 L4 */
u32 ptr_const_volatile(const volatile u32 *p) { return *p + *p; }
/*@ expect: S4 */
void ptr_mmio_struct_index(int i, u32 x) { ((volatile struct regs *)0x40001000)->ch[i] = x; }
/*@ expect: L4 */
u32 ptr_mmio_struct_member(void) { return ((volatile struct regs *)0x40001000)->c; }
/*@ expect: S4 S4 */
void ptr_scalar_member_twice(volatile struct regs *r, u32 x) { r->a = x; r->a = x; }
/*@ expect: S4 S4 */
void ptr_scalar_member_b_twice(volatile struct regs *r, u32 x) { r->b = x; r->b = x; }
/*@ expect: S4 S4 */
void ptr_array_member_var_index(volatile struct regs *r, u32 x, int i) { r->ch[i] = x; r->ch[i] = x; }
/*@ expect: S4 S4 */
void ptr_array_member_different_values(volatile struct regs *r) { r->ch[1] = 1; r->ch[1] = 2; }
volatile struct regs gr;
/*@ expect: S4 S4 */
void ptr_global_struct_array_member(u32 x) { gr.ch[1] = x; gr.ch[1] = x; }
/*@ expect: S4 S4 */
void ptr_volatile_element_array(volatile u32 (*a)[4], u32 x) { (*a)[1] = x; (*a)[1] = x; }
struct wrap { volatile u32 ch[4]; };
/*@ expect: S4 S4 */
void ptr_volatile_element_member(struct wrap *w, u32 x) { w->ch[1] = x; w->ch[1] = x; }
/* A qualifier on the STRUCT reaches the elements of its array members: these
 * lose it at a constant index. */
/*@ expect: S4@16 S4@16 */
void ptr_array_member_const_index(volatile struct regs *r, u32 x) { r->ch[1] = x; r->ch[1] = x; }
/*@ expect: S4@12 S4@12 */
void ptr_array_member_index0(volatile struct regs *r, u32 x) { r->ch[0] = x; r->ch[0] = x; }
/*@ expect: S4 S4 */
void ptr_array_member_same_const(volatile struct regs *r) { r->ch[1] = 5; r->ch[1] = 5; }
/*@ expect: L4@16 L4@16 */
u32 ptr_array_member_two_reads(volatile struct regs *r) { return r->ch[1] + r->ch[1]; }
/*@ expect: S4 S4 */
void ptr_mmio_array_member_twice(u32 x)
{
  ((volatile struct regs *)0x40001000)->ch[1] = x;
  ((volatile struct regs *)0x40001000)->ch[1] = x;
}

/* ======================================================================
 * Aggregates: copying a volatile struct reads all of it.
 * ==================================================================== */

struct P8 { int w[8]; };
/*@ loadwords>=: 2 */
int agg_copy_both_used(volatile struct P2 *vs) { struct P2 t = *vs; return t.a + t.b; }
/*@ loadwords>=: 2 */
int agg_copy_global(void) { struct P2 t = vs2; return t.a; }
/*@ loadwords>=: 8 */
int agg_copy_two_words_used(volatile struct P8 *vs) { struct P8 t = *vs; return t.w[0] + t.w[7]; }
/*@ expect: L4 */
int agg_member_read(volatile struct P2 *vs) { return vs->a; }
/*@ loadwords>=: 2 */
int agg_copy_one_member_used(volatile struct P2 *vs) { struct P2 t = *vs; return t.a; }
/*@ loadwords>=: 2 */
int agg_assign_one_member_used(volatile struct P2 *vs) { struct P2 t; t = *vs; return t.b; }
/*@ loadwords>=: 2 */
int agg_copy_volatile_members(struct V2 *s) { struct V2 t = *s; return t.a; }
/*@ loadwords>=: 2 */
void agg_copy_unused(volatile struct P2 *vs) { struct P2 t = *vs; (void)t; }
/*@ loadwords>=: 8 */
int agg_copy_large_one_used(volatile struct P8 *vs) { struct P8 t = *vs; return t.w[3]; }

/* ======================================================================
 * Control flow.
 * ==================================================================== */

/*@ expect: L4 */
int flow_if(volatile int *p) { if (*p) return 1; return 0; }
/*@ expect: L4 C:ext L4 C:ext */
void flow_no_jump_threading(volatile int *p) { if (*p == 3) ext(); if (*p == 3) ext(); }
/* the switch operand is read once (the second load is the jump table) */
/*@ expect: L4 | L4 L4 */
int flow_switch_sparse(volatile int *p)
{
  switch (*p)
  {
  case 1: return 4;
  case 2: return 7;
  case 3: return 9;
  case 5: return 11;
  default: return 0;
  }
}
/*@ expect: L4 | L4 L4 */
int flow_switch_dense(volatile int *p)
{
  switch (*p)
  {
  case 0: return 4; case 1: return 7; case 2: return 9; case 3: return 1;
  case 4: return 5; case 5: return 2; case 6: return 3; default: return 0;
  }
}
/*@ each: L4 */
void flow_spin_until_zero(volatile int *p) { while (*p != 0) ; }
/*@ expect: L4 */
int flow_conditional_read(volatile int *p, int c) { return c ? *p : 0; }
/*@ expect: S4 S4 */
void flow_store_either_arm(volatile int *p, int c) { if (c) *p = 1; else *p = 2; }
/*@ expect: L4 | L4 L4 */
int flow_read_either_arm(volatile int *p, int c) { int x; if (c) x = *p; else x = *p; return x; }
/*@ expect: L4 L4 */
int flow_reread_on_path(volatile int *p, int c) { int x = *p; if (c) return x; return *p; }
/*@ expect: L4 L4 */
int flow_ternary_reread(volatile int *p) { return *p ? *p : 3; }

/* ======================================================================
 * Volatile locals.
 * ==================================================================== */

/*@ stack ; expect: S4 L4 S4 L4 */
int local_increment(void) { volatile int x = 0; x++; return x; }
/*@ stack ; expect: S4 S4 */
int local_two_stores(void) { volatile int x = 1; x = 2; return 0; }
/*@ stack ; expect: S4 L4 */
int local_store_load(void) { volatile int x; x = 5; return x; }
/*@ stack ; each: S4 L4 C:ext */
void local_counter_loop_with_call(void) { for (volatile int i = 0; i < 1000; i++) ext(); }
/*@ stack ; each: S4 L4 */
void local_delay_variable_bound(int n) { for (volatile int i = 0; i < n; i++) ; }
/*@ stack ; each: S4 L4 */
int local_accumulate(int n) { volatile int acc = 0; for (int i = 0; i < n; i++) acc += i; return acc; }
/*@ stack ; expect: S4 L4 L4 */
int local_volatile_param(volatile int x) { return x + x; }
/*@ expect: L4 S4 L4 */
int local_static_volatile(void) { static volatile int c; c++; return c; }
/*@ expect: S4 L4 */
int local_array_var_index(int k) { volatile int b[4]; b[k] = 1; return b[k]; }
/* busy-wait delay loops with a constant bound */
/*@ stack ; each: S4 L4 */
void local_delay_loop(void) { for (volatile int i = 0; i < 1000; i++) ; }
/*@ stack ; each: S4 L4 */
void local_delay_while(void) { volatile int i = 0; while (i < 1000) i++; }
/*@ stack ; each: S4 L4 */
void local_delay_unsigned(void) { for (volatile u32 i = 0; i < 1000u; i++) ; }
/*@ stack ; each: S4 L4 */
void local_delay_declared_outside(void) { volatile int i; for (i = 0; i < 1000; i++) ; }
/*@ stack ; each: S4 L4 */
void local_delay_short(void) { for (volatile int i = 0; i < 10; i++) ; }
/* volatile local arrays and structs at a constant index / member */
/*@ stack ; expect: S1 L1 */
int local_array_store_load(void) { volatile char b[4]; b[0] = 1; return b[0]; }
/*@ stack ; expect: S4 L4 */
int local_array_single_element(void) { volatile int a[1]; a[0] = 5; return a[0]; }
/*@ stack ; expect: S1 S1 L1 L1 */
int local_array_two_elements(void) { volatile char buf[4]; buf[0] = 1; buf[1] = 2; return buf[0] + buf[1]; }
/*@ stack ; expect: S4 S4 L4 L4 */
int local_struct_members(void) { volatile struct { int a, b; } s; s.a = 1; s.b = 2; return s.a + s.b; }

/* ======================================================================
 * Zig C-backend idioms (CBE output fed to tcc for the kernel).
 * ==================================================================== */

/*@ expect: L4 */
u32 zig_int_to_ptr_offset(uptr base) { return *(u32 volatile *)(base + 8); }
/*@ expect: L4 S4 */
void zig_ptr_from_int_rmw(void) { *(u32 volatile *)(uptr)0x40000000u |= 1; }
/*@ expect: L4 S4 */
void zig_temp_chain_rmw(u32 volatile *t0)
{
  u32 t1 = *t0;
  u32 t2 = t1 | 1;
  *t0 = t2;
}
struct uart { u32 dr, rsr, pad[4], fr; };
/*@ expect: L4 */
u32 zig_direct_field(struct uart volatile *h) { return (*h).fr; }
/*@ expect: L4 */
u32 zig_derived_field_ptr(struct uart volatile *h) { u32 volatile *d = &(*h).fr; return *d; }
/*@ expect: S4 S4 */
void zig_clear_twice(struct uart volatile *h) { (*h).rsr = 0; (*h).rsr = 0; }
/*@ expect: L4 S4 */
void zig_wait_then_write(struct uart volatile *h)
{
  while (((*h).fr & 0x20) != 0) ;
  (*h).dr = 'a';
}
/*@ expect: L4 */
u32 zig_const_ptr_local(void)
{
  struct uart volatile *const t0 = (struct uart volatile *)0x40070000;
  u32 volatile *t1 = &t0->fr;
  u32 t2 = *t1;
  return t2;
}
/* the RP2350 64-bit timer read: hi, lo, hi again until stable */
/*@ expect: L4 L4 L4 */
u64 zig_timer_hi_lo_hi(void)
{
  u32 volatile *hi = (u32 volatile *)0x40054008;
  u32 volatile *lo = (u32 volatile *)0x4005400c;
  u32 h0, l, h1;
  do
  {
    h0 = *hi;
    l = *lo;
    h1 = *hi;
  } while (h0 != h1);
  return (u64)h0 << 32 | l;
}

/*@ expect: L1 */
_Bool misc_bool(volatile _Bool *p) { return *p; }

/* ssa:vrp must not fold a compare that carries a volatile read: a nested
 * `if (*r < n)` is a second mandated read, and `x >=u 0` still reads x. */
/*@ expect: L4 L4 C:ext C:sink */
int vrp_nested_cmp_rereads(volatile int *r, int n)
{
  if (*r < n)
  {
    if (*r < n)
    {
      ext();
      return 1;
    }
    return sink(2);
  }
  return 0;
}
/*@ expect: L4 */
u32 vrp_tautology_cmp_reads(volatile u32 *p) { return *p >= 0; }
/*@ expect: L4 C:sink */
int vrp_tautology_branch_reads(volatile u32 *p)
{
  if (*p <= 0xFFFFFFFFu)
    return sink(1);
  return 0;
}

/* Volatile stores into frame objects nothing reads back are still accesses
 * (dead_local_slot and dse's StackLoc sweep deleted them). */
/*@ stack ; expect: S4 S4 */
void wo_array(void)
{
  volatile int a[4];
  a[0] = 1;
  a[1] = 2;
}
struct wo_S
{
  int a, b;
};
/*@ stack ; expect: S4 S4 */
void wo_struct(void)
{
  volatile struct wo_S s;
  s.a = 4;
  s.b = 5;
}
/*@ stack ; expect: S1 S1 */
void wo_index(int i)
{
  volatile char buf[8];
  buf[0] = 1;
  buf[i] = 2;
}
/*@ stack ; expect: S4 ; O0: S8 S4 */
void wo_scrub(void)
{
  int k[2];
  k[0] = 7;
  k[1] = 8;
  *(volatile int *)&k[0] = 0;
}

/* Aggregate copies INTO a volatile local: vstore's inline LOAD/STORE copies
 * carried the volatile bit on the source side only, so dse deleted the stores
 * (struct word / byte / half copies, complex copies, the register-deref and
 * deref-source paths, and the packed-bitfield unit copy). */
struct agg_S
{
  int a, b;
};
struct agg_C1
{
  unsigned char c;
};
struct __attribute__((packed)) agg_H1
{
  short h;
};
struct __attribute__((packed)) agg_PB
{
  unsigned short a : 5, b : 11;
  unsigned char c;
};
/*@ stack ; expect: S4 S4 S4 S4 ; O0: S8 S8 L4 S4 L4 S4 S8 S8 L4 S4 L4 S4 */
void agg_struct_words(void)
{
  volatile struct agg_S z;
  z = (struct agg_S){1, 2};
  z = (struct agg_S){3, 4};
}
/*@ stack ; expect: S4 S4 S4 S4 ; O0: S8 S8 L4 S4 L4 S4 L4 S4 L4 S4 */
void agg_struct_words_params(struct agg_S x, struct agg_S y)
{
  volatile struct agg_S z;
  z = x;
  z = y;
}
/*@ stack ; expect: S1 S1 ; O0: S1 S1 S1 S1 L1 S1 L1 S1 */
void agg_struct_byte(void)
{
  volatile struct agg_C1 z;
  struct agg_C1 a = {1}, b = {2};
  z = a;
  z = b;
}
/*@ stack ; expect: S2 S2 ; O0: S1 S1 S2 S1 S1 S2 L2 S2 L2 S2 */
void agg_struct_half(void)
{
  _Alignas(2) volatile struct agg_H1 z;
  _Alignas(2) struct agg_H1 a = {1}, b = {2};
  z = a;
  z = b;
}
/*@ stack ; expect: S4 S4 L8 S4 S4 L8 ; O0: S8 S8 L4 S4 L4 S4 L8 L4 S4 L4 S4 L8 */
void agg_complex(_Complex float a, _Complex float b)
{
  volatile _Complex float z;
  z = a;
  z = b;
}
/* q's words are plain reads: pairing them into an LDRD is fine */
/*@ stack ; expect: L4 L4 S4 S4 L4 L4 S4 S4 | L4 L4 S4 S4 L8 S4 S4 | L8 S4 S4 L8 S4 S4 */
void agg_from_deref(struct agg_S *q)
{
  volatile struct agg_S z;
  z = q[0];
  z = q[1];
}
/*@ stack ; expect: S4 S4 S4 S4 ; O0: S8 S8 L4 L4 S4 S4 S8 S8 L4 L4 S4 S4 */
void agg_through_ptr(void)
{
  volatile struct agg_S z;
  volatile struct agg_S *p = &z;
  *p = (struct agg_S){1, 2};
  *p = (struct agg_S){3, 4};
}
/*@ stack ; expect: S2 S1 S2 S1 ; O0: S1 S1 S1 L2 S2 L2 S2 S1 L2 S2 L1 S1 L2 S2 L1 S1 */
void agg_packed_bitfield(void)
{
  _Alignas(4) volatile struct agg_PB z;
  _Alignas(4) struct agg_PB a = {1, 2, 3};
  z = a;
  z = a;
}
/* ...while a non-volatile local's dead copies still go. */
/*@ stack ; expect: ; O0: S8 S8 L4 S4 L4 S4 S8 S8 L4 S4 L4 S4 */
void agg_nonvolatile_dead(void)
{
  struct agg_S z;
  z = (struct agg_S){1, 2};
  z = (struct agg_S){3, 4};
}
/* A copy too big to expand inline is a memmove call; the volatile destination
 * must keep that call even though nothing reads it back. */
struct agg_B
{
  int a[100];
};
/*@ expect: Lm3 Sm3 L4@0 S4@0 | C:__aeabi_memmove4 */
void agg_big_memmove(struct agg_B *q)
{
  volatile struct agg_B z;
  z = *q;
}
/* A small packed copy byte-tiles inline instead (the chunk ceiling admits up
 * to eight 1-byte tiles): every byte loaded and stored exactly once, none
 * merged or dropped, volatile destination or not. */
struct __attribute__((packed)) agg_P5
{
  char c[5];
};
/*@ stack ; expect: L1@0 L1@1 L1@2 L1@3 L1@4 S1@0 S1@1 S1@2 S1@3 S1@4 | C:__aeabi_memmove */
void agg_packed5_memmove(struct agg_P5 *q)
{
  volatile struct agg_P5 z;
  z = *q;
}
/*@ stack ; expect: C:__aeabi_memset Lm3 Sm3 L4@0 S4@0 | C:__aeabi_memset C:__aeabi_memmove4 ; O0: C:__aeabi_memset S4@400 S8@404 Lm3 Sm3 L4@0 S4@0 */
void agg_big_memmove_local(int k)
{
  struct agg_B x = {{k, 2, 3}};
  volatile struct agg_B z;
  z = x;
}

/* ======================================================================
 * A function whose only work is a volatile read is not pure: its call is
 * neither deleted when unused nor hoisted out of a loop.
 * ==================================================================== */
extern volatile u32 vt_status;
__attribute__((noinline)) static u32 vt_clr(void) { return vt_status; }
/*@ expect: C:vt_clr | L4 */
void vt_ack(void) { vt_clr(); }

struct vt_R { int a, b, c, d; };
extern volatile int vt_v;
__attribute__((noinline)) static struct vt_R vt_rv(void)
{
  struct vt_R r = {vt_v, 0, 0, 0};
  return r;
}
/*@ expect: C:vt_rv | L4 */
int vt_use_sret(void)
{
  struct vt_R t = vt_rv();
  (void)t;
  return 3;
}
