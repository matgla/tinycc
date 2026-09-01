/*
 * width_demo_main.c - RP2350 driver for the code-size-versus-cycles kernels.
 *
 * The claim this image exists to test is the one that "tcc emits 29% wide
 * instructions and 1.135x the bytes" invites people to make: that byte count
 * ranks implementations by speed.  Each pair in width_kernels.S computes the
 * same checksum from the same array by different instruction selections, and
 * the board reports bytes, instructions and cycles side by side so the three
 * rankings can be read off and compared.
 *
 * Method, and why each piece is there:
 *
 *   - copy_to_ram.  The image runs entirely out of SRAM, so the XIP cache is
 *     not a variable.  That is deliberate and it is also a limitation: from
 *     flash, size *does* buy speed, because a shorter loop is a loop that
 *     stays in cache.  What is isolated here is the core's own cost per
 *     instruction, which is the part the "wide instructions are slow" folklore
 *     is actually about.
 *
 *   - Interrupts off across each timed region.  A SysTick landing inside a
 *     10,000-cycle measurement is a >1% error on a difference that can be
 *     smaller than that.
 *
 *   - Best of REPS.  Same convention as scripts/bench_tcc_suites.py: noise
 *     only ever moves a measurement up, so the minimum is the closest thing to
 *     "what this sequence costs when nothing goes wrong".
 *
 *   - Two datasets.  SMALL values (< 1000) and WIDE values (full uint32).
 *     This is not decoration: Cortex-M33's divider terminates early on
 *     operands with many leading zeros, so a divide benchmark on small inputs
 *     is a divide benchmark with the interesting part removed.  Kernels that
 *     do not divide should score the same on both, which is the harness
 *     checking itself.
 *
 *   - Checksums compared before timings are believed.  A pair whose arms
 *     disagree is reported as MISMATCH and its timing is meaningless.
 *
 * Always built by GCC; the units under test are hand-written assembly, so the
 * compiler has no say in what is being measured.
 */

#include "pico/stdlib.h"
#include "hardware/sync.h"

#include <stdint.h>
#include <stdio.h>

#define UART_ID uart0
#define BAUD_RATE 460800
#define UART_TX_PIN 32
#define UART_RX_PIN 33

/* ARM DWT / DEMCR, spelled out rather than pulled from CMSIS so this file has
 * no dependency beyond the SDK's own headers. */
#define DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define DEMCR_TRCENA (1u << 24)
#define DWT_CYCCNTENA (1u << 0)

#define N_ELEMS 1024u /* multiple of 4: the kernels are 4x unrolled */
#define REPS 9u

typedef unsigned (*kfn_t)(const unsigned *data, unsigned n);

/* Emitted by width_kernels.S: {bytes, instructions} for one unrolled body. */
typedef struct
{
  uint32_t bytes;
  uint32_t instrs;
} kdesc_t;

extern unsigned k_div10_udiv(const unsigned *, unsigned);
extern unsigned k_div10_recip(const unsigned *, unsigned);
extern unsigned k_sum_postinc(const unsigned *, unsigned);
extern unsigned k_sum_addsep(const unsigned *, unsigned);
extern unsigned k_sum_scaled(const unsigned *, unsigned);
extern unsigned k_sum_unscaled(const unsigned *, unsigned);
extern unsigned k_mul45_mul(const unsigned *, unsigned);
extern unsigned k_mul45_shadd(const unsigned *, unsigned);

extern const kdesc_t div10_udiv_desc;
extern const kdesc_t div10_recip_desc;
extern const kdesc_t sum_postinc_desc;
extern const kdesc_t sum_addsep_desc;
extern const kdesc_t sum_scaled_desc;
extern const kdesc_t sum_unscaled_desc;
extern const kdesc_t mul45_mul_desc;
extern const kdesc_t mul45_shadd_desc;

static unsigned data_small[N_ELEMS];
static unsigned data_wide[N_ELEMS];

/* xorshift32: deterministic, so two runs of this image are comparable, and
 * spread across the full word so the WIDE set really is wide. */
static unsigned prng_state = 0x2545F491u;

static unsigned prng_next(void)
{
  unsigned x = prng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  prng_state = x;
  return x;
}

static void fill_data(void)
{
  for (unsigned i = 0; i < N_ELEMS; i++)
  {
    data_wide[i] = prng_next();
    data_small[i] = prng_next() % 1000u;
  }
}

static int enable_cycle_counter(void)
{
  DEMCR |= DEMCR_TRCENA;
  DWT_CYCCNT = 0;
  DWT_CTRL |= DWT_CYCCNTENA;

  uint32_t t0 = DWT_CYCCNT;
  for (volatile int i = 0; i < 100; i++)
    ;
  return DWT_CYCCNT != t0;
}

/* Best-of-REPS cycle count for one kernel over one dataset.  The checksum the
 * kernel returns is handed back so the caller can compare arms. */
static uint32_t measure(kfn_t fn, const unsigned *data, unsigned *out_sum)
{
  uint32_t best = 0xFFFFFFFFu;
  unsigned sum = 0;

  sum = fn(data, N_ELEMS); /* warm-up: settle the SRAM access pattern */

  for (unsigned r = 0; r < REPS; r++)
  {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = DWT_CYCCNT;
    unsigned v = fn(data, N_ELEMS);
    uint32_t t1 = DWT_CYCCNT;
    restore_interrupts(irq);

    uint32_t d = t1 - t0;
    if (d < best)
      best = d;
    sum = v;
  }

  *out_sum = sum;
  return best;
}

/* Cycles per element in thousandths, so the report needs no float printf. */
static uint32_t milli_per_elem(uint32_t cycles)
{
  return (cycles * 1000u + N_ELEMS / 2u) / N_ELEMS;
}

typedef struct
{
  const char *name;
  kfn_t fn;
  const kdesc_t *desc;
} arm_t;

typedef struct
{
  const char *title;
  const char *question;
  arm_t a;
  arm_t b;
  int subtract_skeleton; /* report the arms net of the sum-loop skeleton */
} pair_t;

static const pair_t pairs[] = {
    {"1. unsigned divide by 10",
     "does the shorter sequence win?",
     {"udiv", k_div10_udiv, &div10_udiv_desc},
     {"reciprocal-mul", k_div10_recip, &div10_recip_desc},
     1},
    {"2. array sum: post-indexed load vs separate pointer bump",
     "equal bytes -- one wide instruction against two narrow ones",
     {"ldr [r0],#4", k_sum_postinc, &sum_postinc_desc},
     {"ldr [r0] + add", k_sum_addsep, &sum_addsep_desc},
     0},
    {"3. indexed load: scaled vs unscaled",
     "equal instruction count -- only the encoding width differs",
     {"[r0,r3,lsl#2]", k_sum_scaled, &sum_scaled_desc},
     {"[r0,r3]", k_sum_unscaled, &sum_unscaled_desc},
     0},
    {"4. multiply by 45",
     "is the classic mul->shift-add strength reduction worth anything here?",
     {"muls", k_mul45_mul, &mul45_mul_desc},
     {"shift-add", k_mul45_shadd, &mul45_shadd_desc},
     1},
};

#define N_PAIRS (sizeof(pairs) / sizeof(pairs[0]))

static void print_milli(uint32_t m)
{
  printf("%u.%03u", (unsigned)(m / 1000u), (unsigned)(m % 1000u));
}

/* One arm's row.  `base_*` are the skeleton costs to subtract, or 0. */
static void report_arm(const arm_t *arm, uint32_t c_small, uint32_t c_wide, unsigned s_small,
                       unsigned s_wide, uint32_t base_small, uint32_t base_wide)
{
  printf("    %-16s %2u B  %u instr   small=", arm->name, (unsigned)arm->desc->bytes,
         (unsigned)arm->desc->instrs);
  print_milli(milli_per_elem(c_small));
  printf("  wide=");
  print_milli(milli_per_elem(c_wide));

  if (base_small || base_wide)
  {
    printf("  net=");
    print_milli(milli_per_elem(c_small - base_small));
    printf("/");
    print_milli(milli_per_elem(c_wide - base_wide));
  }

  printf("  sum=%08X/%08X\r\n", s_small, s_wide);
}

/* Ratio b/a in hundredths, printed as x.xx. */
static void print_ratio(const char *label, uint32_t a, uint32_t b)
{
  if (a == 0)
    return;
  uint32_t r = (b * 100u + a / 2u) / a;
  printf("%s%u.%02u", label, (unsigned)(r / 100u), (unsigned)(r % 100u));
}

int main(void)
{
  stdio_init_all();

  uart_init(UART_ID, BAUD_RATE);
  gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

  uart_tx_wait_blocking(UART_ID);
  sleep_ms(100);

  for (int i = 0; i < 20; i++)
    uart_putc_raw(UART_ID, '~');
  printf("\r\n");
  uart_tx_wait_blocking(UART_ID);
  sleep_ms(50);

  /* Same preamble the benchmark harness uses, so run_benchmark.py's capture
   * script syncs on this image unchanged. */
  printf("===SYNC_START===\r\n");
  uart_tx_wait_blocking(UART_ID);

  printf("width demo: code size vs cycles, Cortex-M33 (RP2350), code+data in SRAM\r\n");
  printf("elements=%u  best-of=%u  irqs off during measurement\r\n\r\n", (unsigned)N_ELEMS,
         (unsigned)REPS);

  if (!enable_cycle_counter())
  {
    printf("WIDTH DEMO: FAIL DWT_CYCCNT does not count\r\n");
    printf("benchmark stopped\r\n");
    uart_tx_wait_blocking(UART_ID);
    while (1)
      tight_loop_contents();
  }

  fill_data();

  /* The skeleton: cases 1 and 4 are this loop with one operation spliced in,
   * so subtracting it isolates the divide and the multiply from the load and
   * the accumulate they both carry. */
  unsigned base_ss, base_sw;
  uint32_t base_small = measure(k_sum_postinc, data_small, &base_ss);
  uint32_t base_wide = measure(k_sum_postinc, data_wide, &base_sw);

  printf("skeleton (ldr postinc + add), subtracted from cases 1 and 4: ");
  print_milli(milli_per_elem(base_small));
  printf(" cyc/elem\r\n\r\n");

  int mismatches = 0;

  for (unsigned i = 0; i < N_PAIRS; i++)
  {
    const pair_t *p = &pairs[i];
    unsigned as_s, as_w, bs_s, bs_w;

    uint32_t ac_s = measure(p->a.fn, data_small, &as_s);
    uint32_t ac_w = measure(p->a.fn, data_wide, &as_w);
    uint32_t bc_s = measure(p->b.fn, data_small, &bs_s);
    uint32_t bc_w = measure(p->b.fn, data_wide, &bs_w);

    printf("  %s\r\n", p->title);
    printf("    (%s)\r\n", p->question);

    uint32_t sub_s = p->subtract_skeleton ? base_small : 0;
    uint32_t sub_w = p->subtract_skeleton ? base_wide : 0;

    report_arm(&p->a, ac_s, ac_w, as_s, as_w, sub_s, sub_w);
    report_arm(&p->b, bc_s, bc_w, bs_s, bs_w, sub_s, sub_w);

    int same = (as_s == bs_s) && (as_w == bs_w);
    if (!same)
      mismatches++;

    printf("    verdict: %s", same ? "MATCH" : "*** MISMATCH ***");

    if (same)
    {
      int32_t dbytes = (int32_t)p->b.desc->bytes - (int32_t)p->a.desc->bytes;
      printf("   size: %s by %d B", dbytes > 0 ? p->a.name : (dbytes < 0 ? p->b.name : "tie"),
             (int)(dbytes < 0 ? -dbytes : dbytes));

      print_ratio("   cycles(wide) b/a: ", ac_w, bc_w);
      printf("  -> %s", bc_w < ac_w ? p->b.name : (bc_w > ac_w ? p->a.name : "tie"));
    }
    printf("\r\n\r\n");
  }

  printf("WIDTH DEMO: %s mismatches=%d\r\n", mismatches ? "FAIL" : "PASS", mismatches);

  /* run_benchmark.py's capture script stops reading on this exact phrase. */
  printf("benchmark stopped\r\n");
  uart_tx_wait_blocking(UART_ID);

  while (1)
    tight_loop_contents();
}
