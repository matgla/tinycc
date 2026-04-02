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

#define THUMB_REGULAR_VARIANT(tok) #tok "eq"
#define THUMB_SETFLAGS_VARIANT(tok) #tok "seq"

/* DEPRECATED: These macros are obsolete after token refactoring */
/* Kept temporarily for reference during transition - DO NOT USE */
#define THUMB_INSTRUCTION_GROUP(tok) ((((tok) - TOK_ASM_nop) & 0xFFFFFFC0) + TOK_ASM_nop)

#define THUMB_HAS_WIDE_QUALIFIER(tok)                                                                                  \
  ((tok - THUMB_INSTRUCTION_GROUP(tok)) > 0x0f && (tok - THUMB_INSTRUCTION_GROUP(tok)) <= 0x1f)

#define THUMB_HAS_NARROW_QUALIFIER(tok)                                                                                \
  ((tok - THUMB_INSTRUCTION_GROUP(tok)) >= 0x1f && (tok - THUMB_INSTRUCTION_GROUP(tok)) <= 0x2f)

#define THUMB_IS_CONDITIONAL(tok)                                                                                      \
  ((tok - THUMB_INSTRUCTION_GROUP(tok)) >= 0x01 && (tok - THUMB_INSTRUCTION_GROUP(tok)) <= 0x0e)

#define THUMB_GET_CONDITION(tok) ((tok - THUMB_INSTRUCTION_GROUP(tok)) % 16)

#define THUMB_IS_SETFLAGS(group, tok) ((tok - group) == 0x40)

/* New simplified macro - single token per instruction */
/* Condition codes and width qualifiers are now parsed at runtime */
#define DEF_ASM_BASE(x) DEF(TOK_ASM_##x, #x)

/* Old macros - now just wrappers around DEF_ASM_BASE for compatibility */
#define DEF_ASM_CONDED(x) DEF_ASM_BASE(x)
#define DEF_ASM_CONDED_WITH_QUALIFIER(x) DEF_ASM_BASE(x)
#define DEF_ASM_CONDED_WITH_SUFFIX(x, y) DEF_ASM_BASE(x)
#define DEF_ASM_CONDED_VFP_F32_F64(x) DEF_ASM_BASE(x)
#define DEF_ASM_CONDED_WITH_TWO_SUFFIXES(x, y, z) DEF_ASM_BASE(x)

/* Note: add new tokens after nop (MUST always use DEF_ASM_BASE) */

DEF_ASM_BASE(nop)
DEF_ASM_BASE(sev)
DEF_ASM_BASE(wfi)
DEF_ASM_BASE(wfe)
DEF_ASM_BASE(yield)

// Data manipulation instructions
DEF_ASM_BASE(adc)
DEF_ASM_BASE(adcs)

DEF_ASM_BASE(add)
DEF_ASM_BASE(adds)

DEF_ASM_BASE(and)
DEF_ASM_BASE(ands)
DEF_ASM_BASE(addw)

DEF_ASM_BASE(bfc)
DEF_ASM_BASE(bfi)

DEF_ASM_BASE(bic)
DEF_ASM_BASE(bics)

DEF_ASM_BASE(clz)
DEF_ASM_BASE(cmn)

DEF_ASM_BASE(eor)
DEF_ASM_BASE(eors)

DEF_ASM_BASE(mvn)
DEF_ASM_BASE(mvns)

DEF_ASM_BASE(orn)
DEF_ASM_BASE(orns)

DEF_ASM_BASE(orr)
DEF_ASM_BASE(orrs)

DEF_ASM_BASE(rsb)
DEF_ASM_BASE(rsbs)

DEF_ASM_BASE(sbc)
DEF_ASM_BASE(sbcs)

DEF_ASM_BASE(sbfx)

DEF_ASM_BASE(rbit)
DEF_ASM_BASE(revsh)
DEF_ASM_BASE(rev)
DEF_ASM_BASE(rev16)

DEF_ASM_BASE(ror)
DEF_ASM_BASE(rors)

DEF_ASM_BASE(lsl)
DEF_ASM_BASE(lsls)

DEF_ASM_BASE(lsr)
DEF_ASM_BASE(lsrs)

DEF_ASM_BASE(asr)
DEF_ASM_BASE(asrs)

DEF_ASM_BASE(rrx)
DEF_ASM_BASE(rrxs)

DEF_ASM_BASE(pkhbt)
DEF_ASM_BASE(pkhtb)

DEF_ASM_BASE(mov)
DEF_ASM_BASE(movs)
DEF_ASM_BASE(movt)
DEF_ASM_BASE(movw)
DEF_ASM_BASE(mrs)
DEF_ASM_BASE(msr)
// Addressing instructions

DEF_ASM_BASE(adr)

DEF_ASM_BASE(cmp)

DEF_ASM_BASE(push)
DEF_ASM_BASE(pop)

// control instructions
DEF_ASM_BASE(clrex)
DEF_ASM_BASE(bkpt)
DEF_ASM_BASE(svc)
DEF_ASM_BASE(cpsid)
DEF_ASM_BASE(cpsie)
DEF_ASM_BASE(csdb)
DEF_ASM_BASE(dmb)
DEF_ASM_BASE(dsb)
DEF_ASM_BASE(isb)
DEF_ASM_BASE(ssbb)
DEF_ASM_BASE(tt)
DEF_ASM_BASE(ttt)
DEF_ASM_BASE(tta)
DEF_ASM_BASE(ttat)
DEF_ASM_BASE(udf)

DEF_ASM_BASE(b)
DEF_ASM_BASE(bl)
DEF_ASM_BASE(bx)
DEF_ASM_BASE(blx)
DEF_ASM_BASE(cbz)
DEF_ASM_BASE(cbnz)
DEF_ASM_BASE(tbb)
DEF_ASM_BASE(tbh)
DEF_ASM_BASE(teq)
DEF_ASM_BASE(tst)

// memory access instructions
DEF_ASM_BASE(lda)
DEF_ASM_BASE(ldab)
DEF_ASM_BASE(ldaex)
DEF_ASM_BASE(ldaexb)
DEF_ASM_BASE(ldaexh)
DEF_ASM_BASE(ldah)
DEF_ASM_BASE(ldm)
DEF_ASM_BASE(ldmfd)
DEF_ASM_BASE(ldmia)

DEF_ASM_BASE(ldmdb)
DEF_ASM_BASE(ldmea)

DEF_ASM_BASE(ldr)
DEF_ASM_BASE(ldrb)
DEF_ASM_BASE(ldrbt)
DEF_ASM_BASE(ldrd)
DEF_ASM_BASE(ldrex)
DEF_ASM_BASE(ldrexb)
DEF_ASM_BASE(ldrexh)
DEF_ASM_BASE(ldrh)
DEF_ASM_BASE(ldrht)
DEF_ASM_BASE(ldrsb)
DEF_ASM_BASE(ldrsbt)
DEF_ASM_BASE(ldrsh)
DEF_ASM_BASE(ldrsht)
DEF_ASM_BASE(ldrt)
DEF_ASM_BASE(pld)
DEF_ASM_BASE(pldw)
DEF_ASM_BASE(pli)
DEF_ASM_BASE(pliw)

DEF_ASM_BASE(stl)
DEF_ASM_BASE(stlb)
DEF_ASM_BASE(stlex)
DEF_ASM_BASE(stlexb)
DEF_ASM_BASE(stlexh)
DEF_ASM_BASE(stlh)
DEF_ASM_BASE(stm)
DEF_ASM_BASE(stmia)
DEF_ASM_BASE(stmea)
DEF_ASM_BASE(stmdb)
DEF_ASM_BASE(stmfd)
DEF_ASM_BASE(str)
DEF_ASM_BASE(strb)
DEF_ASM_BASE(strbt)
DEF_ASM_BASE(strd)
DEF_ASM_BASE(strex)
DEF_ASM_BASE(strexb)
DEF_ASM_BASE(strexh)
DEF_ASM_BASE(strh)
DEF_ASM_BASE(strht)
DEF_ASM_BASE(strt)
DEF_ASM_BASE(sub)
DEF_ASM_BASE(subs)
DEF_ASM_BASE(subw)
DEF_ASM_BASE(sxtb)
DEF_ASM_BASE(sxth)
DEF_ASM_BASE(uxtb)
DEF_ASM_BASE(uxth)

DEF_ASM_BASE(mla)
DEF_ASM_BASE(mls)

DEF_ASM_BASE(mul)
DEF_ASM_BASE(muls)
DEF_ASM_BASE(sdiv)
DEF_ASM_BASE(smlal)
DEF_ASM_BASE(smull)
DEF_ASM_BASE(ssat)
DEF_ASM_BASE(udiv)
DEF_ASM_BASE(umlal)
DEF_ASM_BASE(umull)
DEF_ASM_BASE(usat)

/* DSP byte-parallel instructions (ARMv7E-M / ARMv8-M Mainline) */
DEF_ASM_BASE(uadd8)
DEF_ASM_BASE(usub8)
DEF_ASM_BASE(sel)

/* floating point */
DEF_ASM_BASE(vpush)
DEF_ASM_BASE(vpop)
DEF_ASM_BASE(vadd)
DEF_ASM_BASE(vsub)
DEF_ASM_BASE(vmul)
DEF_ASM_BASE(vdiv)
DEF_ASM_BASE(vneg)
DEF_ASM_BASE(vcmp)
DEF_ASM_BASE(vmov)
DEF_ASM_BASE(vmrs)

/* multiplication */
