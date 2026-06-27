/* Force references to ARM EABI soft-float helpers. */
volatile double gd;
volatile float gf;

int force_softfp(void) {
    gd = gd + 1.5;
    gd = gd * 2.5;
    gf = gf + 1.5f;
    gf = gf * 2.5f;
    return (int)gd + (int)gf;
}
