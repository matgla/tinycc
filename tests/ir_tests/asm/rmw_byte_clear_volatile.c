struct BF { unsigned a : 8, b : 8, c : 16; };
void clr_word(volatile unsigned *p) { *p &= 0xFFFFFF00u; }
void clr_bf(volatile struct BF *p) { p->a = 0; }
void clr_bf2(volatile struct BF *p) { p->a = 0; p->b = 0; }
void clr_plain(unsigned *p) { *p &= 0xFFFFFF00u; }
