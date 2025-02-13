static void th_b_t1(uint16_t cond, uint16_t imm8)
{
    const uint16_t masked_cond = cond & 0xf;
    const uint16_t masked_imm8 = imm8 & 0xff;
    ot(0xd000 | masked_cond | masked_imm8);
}

static void th_b_t2(uint16_t imm11)
{
    const uint16_t masked_imm11 = 0x7f;
    ot(0xe000 | masked_imm11);
}
