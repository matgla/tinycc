/*
 * varargs fuzz seed 31282 reduction (O1/O2 wrong-code):
 * const_var_prop exposed a variadic call with stack-passed anonymous args to
 * a backend/register-allocation miscompile.  Guard the ABI-sensitive shape so
 * O1/O2 continue to match O0/gcc.
 * Ground truth for this reduced repro: checksum=226907cb
 */
#include <stdarg.h>
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

static unsigned vsum(unsigned n, ...)
{
  va_list ap;
  unsigned acc = 0u;
  unsigned i;
  return acc;
}

struct S {
};

int main(void)
{
  unsigned cs = 0x12345678u;
  int s1 = (int)(1525303634u & 0xffffffff);
  int s2 = (int)(1689323415u & 0xffffffff);
  unsigned u3 = 3813756115u;
  unsigned u4 = 3978630896u;
  unsigned arr5[8] = {2257865126u, 4201288696u, 2649654095u, 1464037544u,
                      568664409u,  2734840029u, 2596701301u, 877302385u};
  unsigned arr6[8] = {4156569425u, 1920166371u, 4150015144u, 1226592494u,
                      2949571075u, 3883113236u, 2420159093u, 2773574537u};

  cs = csmix(cs, (unsigned)((~((unsigned)(arr6[((unsigned)(u4) & 7u)]) | 0u))));
  cs = csmix(cs, vsum(4u, (int)(((unsigned)(((unsigned)(263590034u) |
                                             (unsigned)(arr6[((unsigned)(u3) & 7u)]))) ^
                                 (unsigned)(arr6[((unsigned)(u3) & 7u)]))),
                       (int)(u4), (int)(128384070u), (int)((unsigned)(s2))));
  for (unsigned g8 = 0u; g8 < 2u; g8++) {
    unsigned i7 = g8;
    cs = csmix(cs, i7);
    cs = csmix(cs, (unsigned)((~((unsigned)(1170369644u) | 0u))));
    i7 = (unsigned)(((unsigned)((-((unsigned)(((unsigned)(528798870u) -
                                               (unsigned)(1276851323u))) |
                                  0u))) *
                     (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) +
                                                        (unsigned)(i7))) -
                                             (unsigned)(((unsigned)(arr5[((unsigned)(u4) & 7u)]) -
                                                        (unsigned)(3355717364u))))) !=
                                 ((unsigned)(((unsigned)(2589080299u) *
                                              (unsigned)(arr5[((unsigned)(1173880659u) & 7u)]))) ^
                                  cs))))) &
         0xffffffffu;
    cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s1)) + (unsigned)(i7))));
    cs = csmix(cs,
               vsum(7u,
                    (int)((-((unsigned)((~((unsigned)(((unsigned)((~((unsigned)(i7) | 0u))) |
                                                       (unsigned)(((unsigned)(3000295694u) -
                                                                  (unsigned)(1193006265u))))) |
                                           0u))) |
                           0u))),
                    (int)(((unsigned)(((unsigned)(arr5[((unsigned)(i7) & 7u)]) *
                                      (unsigned)(378662670u))) >>
                           ((unsigned)(834032692u) & 31u))),
                    (int)(((unsigned)(i7) +
                           (unsigned)(((unsigned)(4095848160u) /
                                       ((unsigned)((((unsigned)(((unsigned)((unsigned)(s1)) |
                                                              (unsigned)(arr5[((unsigned)(u4) & 7u)]))) &
                                                    1u)
                                                       ? (unsigned)(((unsigned)(u4) >>
                                                                    ((unsigned)(i7) & 31u)))
                                                       : (unsigned)(i7))) |
                                        1u))))),
                    (int)(((unsigned)(((unsigned)((unsigned)(s2)) %
                                      ((unsigned)(((unsigned)(((unsigned)(arr5[((unsigned)(733844087u) & 7u)]) &
                                                             (unsigned)(403975553u))) <<
                                                   ((unsigned)(arr5[((unsigned)(3476630399u) & 7u)]) &
                                                    31u))) |
                                       1u))) |
                           (unsigned)(((unsigned)(u3) -
                                      (unsigned)(((unsigned)((-((unsigned)(u3) | 0u))) /
                                                 ((unsigned)((-((unsigned)(2643543317u) | 0u))) |
                                                  1u))))))),
                    (int)(((unsigned)(1744362987u) -
                           (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) /
                                                             ((unsigned)(3621839285u) | 1u))) >>
                                                   ((unsigned)(2365832682u) & 31u))) &
                                      (unsigned)(((unsigned)(2748244799u) <<
                                                  ((unsigned)(u3) & 31u))))))),
                    (int)(((unsigned)(((unsigned)(((unsigned)(137163279u) <<
                                                  ((unsigned)((((unsigned)(u4) & 1u)
                                                                 ? (unsigned)((unsigned)(s2))
                                                                 : (unsigned)(arr5[((unsigned)(1800070983u) & 7u)]))) &
                                                   31u))) <<
                                      ((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) ^
                                                             (unsigned)(arr6[((unsigned)(i7) & 7u)]))) %
                                                   ((unsigned)((~((unsigned)(i7) | 0u))) | 1u))) &
                                       31u))) |
                           (unsigned)((-((unsigned)(arr6[((unsigned)(u4) & 7u)]) | 0u))))),
                    (int)(((unsigned)(u4) >> ((unsigned)((unsigned)(s2)) & 31u)))));
    {
      unsigned g10 = 0u;
      while (g10 < 1u) {
        unsigned i9 = g10;
        cs = csmix(cs, i9);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(i9) +
                                               (unsigned)(1267291956u))) <=
                                   ((unsigned)(((unsigned)(3231997240u) +
                                                (unsigned)(u4))) ^
                                    cs))));
        cs = csmix(cs, vsum(0u));
        g10++;
      }
    }
  }
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, vsum(2u, 1, (int)cs));
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  for (unsigned k = 0u; k < 8u; k++)
    cs = csmix(cs, arr5[k]);
  for (unsigned k = 0u; k < 8u; k++)
    cs = csmix(cs, arr6[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
