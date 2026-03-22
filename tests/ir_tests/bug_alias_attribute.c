static int base1(void)
{
  return 11;
}

extern int alias1(void) __attribute__((__alias__("base1")));

static int base2(void) __asm__("asm_base2");
static int base2(void)
{
  return 22;
}

extern int alias2(void) __attribute__((__alias__("asm_base2")));

static int base3(void);
extern int alias3(void) __attribute__((__alias__("base3")));
static int base3(void)
{
  return 33;
}

static int data1 = 44;
extern int data1_alias __attribute__((__alias__("data1")));

static int data2 __asm__("asm_data2") = 55;
extern int data2_alias __attribute__((__alias__("asm_data2")));

extern int data3_alias __attribute__((__alias__("data3")));
static int data3 = 66;

int main(void)
{
  return !(alias1() == 11 && alias2() == 22 && alias3() == 33 && data1_alias == 44 && data2_alias == 55 &&
           data3_alias == 66);
}