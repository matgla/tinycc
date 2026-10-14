/* Integer complex constants assigned to narrower integer complex objects:
 * the conversion sized each part like a float (4 bytes), so a `_Complex
 * short` local got its imaginary part past its end; and the converted
 * parts went into the frontend's temporary slot as word-wide ASSIGNs,
 * clobbering whatever frame relayout packed after it. */
#include <stdio.h>
_Complex unsigned short gus = 1 + 65535i;
_Complex short gss = 1 + 1000i;
_Complex unsigned char guc = 2 + 200i;
_Complex int gi = 3 + 65535i;
int main(void){
  _Complex unsigned short us = 1 + 65535i;
  _Complex short ss = 1 + 1000i;
  _Complex unsigned char uc = 2 + 200i;
  _Complex unsigned short us2; us2 = 4 + 7i;
  printf("%u %u | %d %d | %u %u | %d %d\n", (unsigned)__real__ gus, (unsigned)__imag__ gus, __real__ gss, __imag__ gss, (unsigned)__real__ guc, (unsigned)__imag__ guc, __real__ gi, __imag__ gi);
  printf("%u %u | %d %d | %u %u | %u %u\n", (unsigned)__real__ us, (unsigned)__imag__ us, __real__ ss, __imag__ ss, (unsigned)__real__ uc, (unsigned)__imag__ uc, (unsigned)__real__ us2, (unsigned)__imag__ us2);
  return 0;
}
