/* CMP Rn, #-k with k = 1..7 is ADDS Rt, Rn, #k into a free low register:
 * the same N, Z, C and V in 16 bits (test_cmp_negative_small_is_adds). */
extern int hit(void);
extern int miss(void);

int is_none(unsigned x)
{
  return x == 0xFFFFFFFFu ? hit() : miss();
}

int below_minus3(int x)
{
  return x < -3 ? hit() : miss();
}
