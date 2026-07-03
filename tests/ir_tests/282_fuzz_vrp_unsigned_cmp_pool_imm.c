/*
 * ptr fuzz seed 35289 reduction (O1/O2): VRP models 32-bit values as
 * sign-extended int32 (the IMM32 operand encoding), but the CMP immediate
 * that const-prop folded from `1261003109u ^ lr` (= 3435266601) arrived as a
 * pool-stored I64 holding the ZERO-extended value.  vrp's CMP+SETIF fold then
 * compared the sign-extended range endpoint (-2026822809 → uint64
 * 0xFFFFFFFF87...) against the zero-extended immediate (0xCCC5FBA9) and
 * folded the unsigned `<` to 0 when the true 32-bit answer is 1, flipping
 * helper2's returned checksum bit.
 * Fixed by normalizing every constant entering vrp's range machinery to the
 * sign-extended int32 domain (vrp_read_const32 in ir/opt_branch.c) and
 * enforcing vrp_fold_cmp's same-sign precondition for unsigned compares.
 * Ground truth (tcc -O0 == gcc -O2): checksum=846c7a1c.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(2280350540u);
  return (unsigned)(((unsigned)(((unsigned)(4282761243u) + (unsigned)(((unsigned)(lr) << ((unsigned)(((unsigned)(lr) ^ lr)) & 31u))))) < ((unsigned)(1261003109u) ^ lr))) ^ lr;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  cs = csmix(cs, helper2(19088744u, cs));
  printf("checksum=%08x\n", cs);
  return 0;
}
