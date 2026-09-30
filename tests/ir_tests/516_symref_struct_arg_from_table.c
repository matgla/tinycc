/* A struct passed by value out of a constant local spec -- the opt DSL's
   `const IROptRewriteSpec _opt_dsl_rw = {...}` handing `_opt_dsl_rw.src1` to a
   setter.  const_local_table reads the spec from .rodata, so the argument is
   `*(image + 4)`, a STRUCT-typed symref operand.  symref_prop rebuilt such
   operands with the index in u.pool_idx, but a STRUCT operand keeps its
   ctype in u.s.ctype_idx and the symref index in u.s.aux_data: the argument
   came from whatever symbol aux_data last named (the self-hosted tcc read
   tcc_ir_op_get_dest's GOT slot and crashed). */
#include <stdio.h>
#include <stdint.h>

struct op { uint32_t a, b, c; };
struct spec { uint32_t tag; struct op d, s1; uint8_t flags; };

__attribute__((noinline)) static uint32_t take(struct op o) { return o.a * 100 + o.b * 10 + o.c; }
__attribute__((noinline)) static uint32_t take2(uint32_t k, struct op o) { return k + take(o); }

__attribute__((noinline)) static uint32_t apply(int which, uint32_t k)
{
    const struct spec sp = {7, {1, 2, 3}, {4, 5, 6}, 9};
    uint32_t r = sp.tag;
    if (which & 1)
        r += take(sp.d);
    if (which & 2)
        r += take2(k, sp.s1);
    if (sp.flags == 9)
        r += take(sp.s1);
    return r;
}

int main(void)
{
    printf("apply %u %u %u %u\n", apply(0, 5), apply(1, 5), apply(2, 5), apply(3, 1000));
    return 0;
}
