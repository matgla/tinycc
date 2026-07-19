/* Small constant-size mem* inline expansion (source/opt/flat/memory/mem_inline.c).
 *
 * Strict static-win policy: only single-piece memcpy (n in {1,2,4}) and
 * <=2-piece memset expand; larger sizes keep the runtime call (measured:
 * gcc-mirroring thresholds cost +46k corpus instructions for ~zero cycles).
 *
 * The expanded word accesses go through possibly-unaligned pointers, so they
 * must stay on unaligned-tolerant LDR/STR: the UNDERALIGN hint has to survive
 * to codegen and block the LDRD/STRD pairing peepholes (which fault on
 * unaligned addresses regardless of CCR.UNALIGN_TRP). */

typedef unsigned int size_t;
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);

/* n == 4, single piece: ldr + str, no call. */
void cp4(char *d, char *s) { memcpy(d, s, 4); }

/* n == 8: two pieces -> stays a call under the strict policy. */
void cp8(char *d, char *s) { memcpy(d, s, 8); }

/* n == 8 memset, two word stores, no call, and NO strd (char* dest). */
void st8(char *d) { memset(d, 0, 8); }

/* n == 0: the call disappears entirely. */
void cp0(char *d, char *s) { memcpy(d, s, 0); }

/* union-reinterpret idiom: the 4-byte copy expands and forwards. */
unsigned fbits(float f)
{
  unsigned u;
  memcpy(&u, &f, sizeof u);
  return u;
}
