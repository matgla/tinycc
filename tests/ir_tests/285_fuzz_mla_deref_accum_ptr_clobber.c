/*
 * ptr fuzz seed 59549 reduction (O2 HardFault): the MLA emitter
 * (tcc_gen_machine_mla_mop) pre-excluded only NON-deref REG operands, but a
 * dereferenced operand's r0 is its POINTER register.  For
 * (*p10 * st12.f0) + (*p10) the accumulator was *p10 (deref, pointer in
 * r0); materialising src2 (st12.f0, spilled) picked r0 as the reload
 * scratch, so the accumulator deref read *(st12.f0) = *(0x8A4CB157) --
 * precise bus fault (CFSR=0x8200, BFAR=0x8A4CB157).
 * Fixed by excluding deref operands' pointer registers in the MLA emitter
 * (and the same latent gap in tcc_gen_machine_mlal_accum_mop).
 * Ground truth (tcc -O0 == gcc -O2): checksum=1340a3d9.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}


struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  long s1 = (long)(875599349u & 0xffffffff);
  long s3 = (long)(1849938066u & 0xffffffff);
  unsigned u4 = 3350806346u;
  unsigned u5 = 4064692390u;
  unsigned arr7[8] = { 383113438u, 3203720828u, 1307113321u, 196959331u, 1152839551u, 1572238572u, 631132936u, 3928801857u };
  unsigned *p8 = &arr7[0u];
  unsigned *p9 = &arr7[0u];
  unsigned *p10 = &arr7[((unsigned)(u5) & 7u)];
  unsigned *p11 = &arr7[0u];
  struct S st12 = { 2320281943u, 1220001509u, 4294539779u };

  if ((unsigned)(u5) & 1u) {
  } else {
    { unsigned g16 = 0u;
      while (g16 < 6u) {
        *p8 = (unsigned)((~((unsigned)((*p9)) | 0u)));
        cs = csmix(cs, *p10);
        *p10 = (unsigned)(((unsigned)(((unsigned)((-((unsigned)(u5) | 0u))) == ((unsigned)(((unsigned)(((unsigned)(st12.f1) / ((unsigned)(u4) | 1u))) << ((unsigned)((-((unsigned)(3990097843u) | 0u))) & 31u))) ^ cs))) << ((unsigned)(((unsigned)(((unsigned)(((unsigned)(311013508u) - (unsigned)(2294492613u))) << ((unsigned)((unsigned)(s3)) & 31u))) - (unsigned)((~((unsigned)(2829938781u) | 0u))))) & 31u)));
        cs = csmix(cs, *p9);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) & (unsigned)(arr7[((unsigned)(3739498212u) & 7u)]))) ^ (unsigned)(((unsigned)((*p8)) | (unsigned)(3048097719u))))) | (unsigned)(((unsigned)(((unsigned)((*p10)) * (unsigned)(st12.f0))) + (unsigned)((*p10)))))) % ((unsigned)((*p11)) | 1u))));
        g16++;
      }
    }
  }

  cs = csmix(cs, *p11);
  printf("checksum=%08x\n", cs);
  return 0;
}
