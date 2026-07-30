/* value_tracking constant-folds a 64-bit modulo (__aeabi_lmod / __aeabi_ulmod)
 * to its remainder, mirroring the existing __aeabi_ldivmod division fold.
 *
 * At -O0 the runtime helper computes each result; at -O1/-O2/-Os value_tracking
 * replaces the call with the folded constant.  Every optimization level must
 * agree with this .expect output.  Operands are separate locals so the frontend
 * does not fold the modulo itself and the runtime call reaches value_tracking.
 */
#include <stdio.h>

int main(void)
{
  long long a = 1000000000007LL;
  long long b = 13;
  long long c = -13;
  long long na = -1000000000007LL;
  unsigned long long ua = 18446744073709551615ULL;
  unsigned long long ub = 13;

  long long m1 = a % b;             /* signed, positive dividend        */
  long long m2 = na % b;            /* signed, negative dividend        */
  long long m3 = a % c;             /* signed, remainder sign = dividend */
  unsigned long long m4 = ua % ub;  /* unsigned, full 64-bit magnitude  */
  long long m5 = a / b;             /* division sibling, sanity check   */

  printf("m1=%lld m2=%lld m3=%lld m4=%llu m5=%lld\n", m1, m2, m3, m4, m5);
  return 0;
}
