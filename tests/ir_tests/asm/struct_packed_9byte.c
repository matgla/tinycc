/* Phase D: struct by-value 9-byte packed operand handling.
 * AAPCS passes a 9-byte packed struct in registers (r0..r2).  The callee has
 * to reconstruct unaligned fields.  This test pins the current (working) code
 * generation pattern so any future change can be diffed.
 */
struct __attribute__((packed)) S {
    char a;
    int b;
    char c;
    short d;
    char e;
};

int consume(struct S s) {
    return s.b + s.d;
}

struct S make(void);

int caller(void) {
    struct S s = make();
    return consume(s);
}
