/* Force references to ARM EABI integer division/modulo helpers. */
volatile int a;
volatile unsigned b;
volatile long long c;
volatile unsigned long long d;
volatile unsigned long long divisor = 7;

int force_divmod(void) {
    a = a / 7;
    a = a % 7;
    b = b / 7;
    b = b % 7;
    c = c / 7;
    c = c % 7;
    d = d / divisor;
    d = d % divisor;
    return a + b + (int)c + (int)d;
}
