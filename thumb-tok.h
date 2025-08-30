/* ------------------------------------------------------------------ */
/* WARNING: relative order of tokens is important.                    */

/* register */

DEF_ASM(r0)
DEF_ASM(r1)
DEF_ASM(r2)
DEF_ASM(r3)
DEF_ASM(r4)
DEF_ASM(r5)
DEF_ASM(r6)
DEF_ASM(r7)
DEF_ASM(r8)
DEF_ASM(r9)
DEF_ASM(r10)
DEF_ASM(r11) /* fp */
DEF_ASM(r12) /* ip[c] */
DEF_ASM(r13) /* sp */
DEF_ASM(r14) /* lr */
DEF_ASM(r15) /* pc */

/* register macros */

DEF_ASM(fp) /* alias for r11 */
DEF_ASM(ip) /* alias for r12 */
DEF_ASM(sp) /* alias for r13 */
DEF_ASM(lr) /* alias for r14 */
DEF_ASM(pc) /* alias for r15 */

/* single-precision VFP registers */

DEF_ASM(s0)
DEF_ASM(s1)
DEF_ASM(s2)
DEF_ASM(s3)
DEF_ASM(s4)
DEF_ASM(s5)
DEF_ASM(s6)
DEF_ASM(s7)
DEF_ASM(s8)
DEF_ASM(s9)
DEF_ASM(s10)
DEF_ASM(s11)
DEF_ASM(s12)
DEF_ASM(s13)
DEF_ASM(s14)
DEF_ASM(s15)
DEF_ASM(s16)
DEF_ASM(s17)
DEF_ASM(s18)
DEF_ASM(s19)
DEF_ASM(s20)
DEF_ASM(s21)
DEF_ASM(s22)
DEF_ASM(s23)
DEF_ASM(s24)
DEF_ASM(s25)
DEF_ASM(s26)
DEF_ASM(s27)
DEF_ASM(s28)
DEF_ASM(s29)
DEF_ASM(s30)
DEF_ASM(s31)

/* double-precision VFP registers */

DEF_ASM(d0)
DEF_ASM(d1)
DEF_ASM(d2)
DEF_ASM(d3)
DEF_ASM(d4)
DEF_ASM(d5)
DEF_ASM(d6)
DEF_ASM(d7)
DEF_ASM(d8)
DEF_ASM(d9)
DEF_ASM(d10)
DEF_ASM(d11)
DEF_ASM(d12)
DEF_ASM(d13)
DEF_ASM(d14)
DEF_ASM(d15)

/* VFP status registers */

DEF_ASM(fpsid)
DEF_ASM(fpscr)
DEF_ASM(fpexc)

/* VFP magical ARM register */

DEF_ASM(apsr_nzcv)

/* data processing directives */

DEF_ASM(asl)

/* instructions that have no condition code */

/* thumb conditional tokens */
DEF_ASM(it) // must be first
DEF_ASM(itt)
DEF_ASM(ite)
DEF_ASM(ittt)
DEF_ASM(itte)
DEF_ASM(itet)
DEF_ASM(itee)
DEF_ASM(itttt)
DEF_ASM(ittte)
DEF_ASM(ittet)
DEF_ASM(ittee)
DEF_ASM(itett)
DEF_ASM(itete)
DEF_ASM(iteet)
DEF_ASM(iteee) // must be last

#define THUMB_INSTRUCTION_GROUP(tok)                                           \
  ((((tok) - TOK_ASM_nopeq) & 0xFFFFFFC0) + TOK_ASM_nopeq)

#define THUMB_HAS_WIDE_QUALIFIER(tok)                                          \
  ((tok - THUMB_INSTRUCTION_GROUP(tok)) > 0x0f &&                              \
   (tok - THUMB_INSTRUCTION_GROUP(tok)) <= 0x1f)

#define THUMB_HAS_NARROW_QUALIFIER(tok)                                        \
  ((tok - THUMB_INSTRUCTION_GROUP(tok)) >= 0x1f &&                             \
   (tok - THUMB_INSTRUCTION_GROUP(tok)) <= 0x2f)

#define THUMB_IS_CONDITIONAL(tok)                                              \
  ((tok - THUMB_INSTRUCTION_GROUP(tok)) >= 0x01 &&                             \
   (tok - THUMB_INSTRUCTION_GROUP(tok)) <= 0x0e)

#define THUMB_GET_CONDITION(tok) ((tok - THUMB_INSTRUCTION_GROUP(tok)) % 16)

/* Note: condition code is 4 bits */
#define DEF_ASM_CONDED(x)                                                      \
  DEF(TOK_ASM_##x##eq, #x "eq")                                                \
  DEF(TOK_ASM_##x##ne, #x "ne")                                                \
  DEF(TOK_ASM_##x##cs, #x "cs")                                                \
  DEF(TOK_ASM_##x##cc, #x "cc")                                                \
  DEF(TOK_ASM_##x##mi, #x "mi")                                                \
  DEF(TOK_ASM_##x##pl, #x "pl")                                                \
  DEF(TOK_ASM_##x##vs, #x "vs")                                                \
  DEF(TOK_ASM_##x##vc, #x "vc")                                                \
  DEF(TOK_ASM_##x##hi, #x "hi")                                                \
  DEF(TOK_ASM_##x##ls, #x "ls")                                                \
  DEF(TOK_ASM_##x##ge, #x "ge")                                                \
  DEF(TOK_ASM_##x##lt, #x "lt")                                                \
  DEF(TOK_ASM_##x##gt, #x "gt")                                                \
  DEF(TOK_ASM_##x##le, #x "le")                                                \
  DEF(TOK_ASM_##x, #x)                                                         \
  DEF(TOK_ASM_##x##rsvd, #x "rsvd")

/* Note: condition code is 4 bits */
#define DEF_ASM_CONDED_WITH_SUFFIX(x, y)                                       \
  DEF(TOK_ASM_##x##eq##_##y, #x "eq." #y)                                      \
  DEF(TOK_ASM_##x##ne##_##y, #x "ne." #y)                                      \
  DEF(TOK_ASM_##x##cs##_##y, #x "cs." #y)                                      \
  DEF(TOK_ASM_##x##cc##_##y, #x "cc." #y)                                      \
  DEF(TOK_ASM_##x##mi##_##y, #x "mi." #y)                                      \
  DEF(TOK_ASM_##x##pl##_##y, #x "pl." #y)                                      \
  DEF(TOK_ASM_##x##vs##_##y, #x "vs." #y)                                      \
  DEF(TOK_ASM_##x##vc##_##y, #x "vc." #y)                                      \
  DEF(TOK_ASM_##x##hi##_##y, #x "hi." #y)                                      \
  DEF(TOK_ASM_##x##ls##_##y, #x "ls." #y)                                      \
  DEF(TOK_ASM_##x##ge##_##y, #x "ge." #y)                                      \
  DEF(TOK_ASM_##x##lt##_##y, #x "lt." #y)                                      \
  DEF(TOK_ASM_##x##gt##_##y, #x "gt." #y)                                      \
  DEF(TOK_ASM_##x##le##_##y, #x "le." #y)                                      \
  DEF(TOK_ASM_##x##_##y, #x "." #y)                                            \
  DEF(TOK_ASM_##x##rsvd##_##y, #x "rsvd." #y)

#define DEF_ASM_CONDED_VFP_F32_F64(x)                                          \
  DEF_ASM_CONDED_WITH_SUFFIX(x, f32)                                           \
  DEF_ASM_CONDED_WITH_SUFFIX(x, f64)

#define DEF_ASM_CONDED_WITH_TWO_SUFFIXES(x, y, z)                              \
  DEF(TOK_ASM_##x##eq##_##y##_##z, #x "eq." #y "." #z)                         \
  DEF(TOK_ASM_##x##ne##_##y##_##z, #x "ne." #y "." #z)                         \
  DEF(TOK_ASM_##x##cs##_##y##_##z, #x "cs." #y "." #z)                         \
  DEF(TOK_ASM_##x##cc##_##y##_##z, #x "cc." #y "." #z)                         \
  DEF(TOK_ASM_##x##mi##_##y##_##z, #x "mi." #y "." #z)                         \
  DEF(TOK_ASM_##x##pl##_##y##_##z, #x "pl." #y "." #z)                         \
  DEF(TOK_ASM_##x##vs##_##y##_##z, #x "vs." #y "." #z)                         \
  DEF(TOK_ASM_##x##vc##_##y##_##z, #x "vc." #y "." #z)                         \
  DEF(TOK_ASM_##x##hi##_##y##_##z, #x "hi." #y "." #z)                         \
  DEF(TOK_ASM_##x##ls##_##y##_##z, #x "ls." #y "." #z)                         \
  DEF(TOK_ASM_##x##ge##_##y##_##z, #x "ge." #y "." #z)                         \
  DEF(TOK_ASM_##x##lt##_##y##_##z, #x "lt." #y "." #z)                         \
  DEF(TOK_ASM_##x##gt##_##y##_##z, #x "gt." #y "." #z)                         \
  DEF(TOK_ASM_##x##le##_##y##_##z, #x "le." #y "." #z)                         \
  DEF(TOK_ASM_##x##_##y##_##z, #x "." #y "." #z)                               \
  DEF(TOK_ASM_##x##rsvd##_##y##_##z, #x "rsvd." #y "." #z)

/* Note: add new tokens after nop (MUST always use DEF_ASM_CONDED) */

#define DEF_ASM_CONDED_WITH_QUALIFIER(x)                                       \
  DEF_ASM_CONDED(x)                                                            \ 
  DEF_ASM_CONDED_WITH_SUFFIX(x, w) DEF_ASM_CONDED_WITH_SUFFIX(x, n)            \
      DEF_ASM_CONDED_WITH_SUFFIX(x, _) // last just to align to the 6 bits

DEF_ASM_CONDED_WITH_QUALIFIER(nop)

// Data manipulation instructions
DEF_ASM_CONDED_WITH_QUALIFIER(adc)
DEF_ASM_CONDED_WITH_QUALIFIER(adcs)
DEF_ASM_CONDED_WITH_QUALIFIER(add)
DEF_ASM_CONDED_WITH_QUALIFIER(adds)
DEF_ASM_CONDED_WITH_QUALIFIER(addw)
DEF_ASM_CONDED_WITH_QUALIFIER(and)
DEF_ASM_CONDED_WITH_QUALIFIER(ands)
DEF_ASM_CONDED_WITH_QUALIFIER(bfc)
DEF_ASM_CONDED_WITH_QUALIFIER(bfi)
DEF_ASM_CONDED_WITH_QUALIFIER(bic)
DEF_ASM_CONDED_WITH_QUALIFIER(bics)
DEF_ASM_CONDED_WITH_QUALIFIER(clz)
DEF_ASM_CONDED_WITH_QUALIFIER(cmn)
DEF_ASM_CONDED_WITH_QUALIFIER(eor)
DEF_ASM_CONDED_WITH_QUALIFIER(eors)

DEF_ASM_CONDED_WITH_QUALIFIER(lsl)
DEF_ASM_CONDED_WITH_QUALIFIER(lsr)
DEF_ASM_CONDED_WITH_QUALIFIER(asr)
DEF_ASM_CONDED_WITH_QUALIFIER(asrs)
DEF_ASM_CONDED_WITH_QUALIFIER(ror)
DEF_ASM_CONDED_WITH_QUALIFIER(rrx)

DEF_ASM_CONDED_WITH_QUALIFIER(mov)
DEF_ASM_CONDED_WITH_QUALIFIER(movs)
DEF_ASM_CONDED_WITH_QUALIFIER(movt)
DEF_ASM_CONDED_WITH_QUALIFIER(movw)

// Addressing instructions

DEF_ASM_CONDED_WITH_QUALIFIER(adr)

DEF_ASM_CONDED_WITH_QUALIFIER(cmp)

DEF_ASM_CONDED_WITH_QUALIFIER(push)
DEF_ASM_CONDED_WITH_QUALIFIER(pop)

// control instructions
DEF_ASM_CONDED_WITH_QUALIFIER(clrex)
DEF_ASM_CONDED_WITH_QUALIFIER(bkpt)
DEF_ASM_CONDED_WITH_QUALIFIER(svc)
DEF_ASM_CONDED_WITH_QUALIFIER(cpsid)
DEF_ASM_CONDED_WITH_QUALIFIER(cpsie)
DEF_ASM_CONDED_WITH_QUALIFIER(csdb)
DEF_ASM_CONDED_WITH_QUALIFIER(dmb)
DEF_ASM_CONDED_WITH_QUALIFIER(dsb)
DEF_ASM_CONDED_WITH_QUALIFIER(isb)

DEF_ASM_CONDED_WITH_QUALIFIER(b)
DEF_ASM_CONDED_WITH_QUALIFIER(bl)
DEF_ASM_CONDED_WITH_QUALIFIER(bx)
DEF_ASM_CONDED_WITH_QUALIFIER(blx)
DEF_ASM_CONDED_WITH_QUALIFIER(cbz)
DEF_ASM_CONDED_WITH_QUALIFIER(cbnz)

// memory access instructions
DEF_ASM_CONDED_WITH_QUALIFIER(lda)
DEF_ASM_CONDED_WITH_QUALIFIER(ldab)
DEF_ASM_CONDED_WITH_QUALIFIER(ldaex)
DEF_ASM_CONDED_WITH_QUALIFIER(ldaexb)
DEF_ASM_CONDED_WITH_QUALIFIER(ldaexh)
DEF_ASM_CONDED_WITH_QUALIFIER(ldah)
DEF_ASM_CONDED_WITH_QUALIFIER(ldm)
DEF_ASM_CONDED_WITH_QUALIFIER(ldmfd)
DEF_ASM_CONDED_WITH_QUALIFIER(ldmia)

DEF_ASM_CONDED_WITH_QUALIFIER(ldmdb)
DEF_ASM_CONDED_WITH_QUALIFIER(ldmea)

DEF_ASM_CONDED_WITH_QUALIFIER(ldr)

/* multiplication */
