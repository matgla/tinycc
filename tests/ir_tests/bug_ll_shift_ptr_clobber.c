/*
 * Regression test for 64-bit left-shift clobbering an adjacent pointer.
 *
 * Extracted from toybox parse_optflaglist's trailing-group parsing.
 * The inner for-loop has two 64-bit values (ll, bits), a pointer (opt),
 * an index (idx), and an external pointer dereference (*options).
 * The opt->dex[idx] |= bits & ~ll  read-modify-write through an indexed
 * struct member forces TCC into a spill-heavy codegen on ARM where the
 * high word of ll <<= 1 corrupts the opt pointer after 32 iterations.
 *
 * The function deliberately maintains many live variables across the
 * critical loop to exhaust callee-saved registers and force TCC to spill
 * ll to the stack, matching the register pressure in toybox's large
 * parse_optflaglist function.
 */
#include <stdio.h>
#include <string.h>

struct opts {
    struct opts *next;
    long *arg;
    int c;
    int flags;
    unsigned long long dex[3];
    char type;
    union {
        long l;
    } val[3];
};

struct getoptflagstate {
    struct opts *opts;
    unsigned long long requires;
    long *nextarg;
    int argc;
};

#define NNODES 40
static struct opts nodes[NNODES];
static long argslots[NNODES];

/* Prevent the compiler from optimizing away the result. */
static volatile unsigned long long sink_bits;
static volatile unsigned long long sink_dex;
static volatile unsigned long long sink_requires;

static int stridx(const char *s, int c)
{
    int i;
    for (i = 0; s[i]; i++) if (s[i] == c) return i;
    return -1;
}

static void build_list(struct getoptflagstate *gof)
{
    int i;
    for (i = 0; i < NNODES; i++) {
        nodes[i].c = 'A' + i;
        nodes[i].flags = 0;
        nodes[i].type = 0;
        nodes[i].arg = 0;
        nodes[i].next = (i + 1 < NNODES) ? &nodes[i + 1] : (struct opts *)0;
        nodes[i].dex[0] = nodes[i].dex[1] = nodes[i].dex[2] = 0;
        nodes[i].val[0].l = nodes[i].val[1].l = nodes[i].val[2].l = 0;
    }
    gof->opts = &nodes[0];
    gof->requires = 0;
    gof->nextarg = argslots;
    gof->argc = NNODES;
}

/*
 * Simulate parse_optflaglist: first pass assigns dex[1] and args,
 * then the trailing group parsing does the critical for(;;) loop.
 * This keeps gof, nextarg, idx, new, opts all live like the real code.
 *
 * Additional live variables (saveflags, catch, letters, ss) are maintained
 * across the critical loop to exhaust all callee-saved registers and force
 * both ll and opt onto the stack — matching register pressure in the real
 * 600-line parse_optflaglist + get_optflags combined function context.
 */
int main(void)
{
    struct getoptflagstate gof;
    struct opts *new;
    struct opts *catch;
    long *nextarg;
    unsigned long long saveflags;
    char *letters[] = {"s", ""};
    char *ss;
    int idx;
    int rc = 0;

    build_list(&gof);
    nextarg = gof.nextarg;

    /* Phase 1: assign dex/nextarg — mirrors toybox's pre-loop setup.
     * This keeps several variables live and consumed across the function. */
    idx = 0;
    saveflags = 0;
    ss = letters[0];
    for (new = gof.opts; new; new = new->next) {
        unsigned long long u = 1ULL << idx++;
        new->dex[1] = u;
        if (new->flags & 1) gof.requires |= u;
        saveflags |= u;
        if (new->type) {
            new->arg = (void *)nextarg;
            *(nextarg++) = new->val[2].l;
        }
    }

    /* Trailing group: [-BCYZ]
     * Multiple trailing groups like toybox's ls: [-Cxm1][-Cxml][-xm1][-Cxl]
     * More groups = more outer iterations keeping all variables alive longer. */
    char options_buf[] = "[-BCYZ][-BY][-CZ]";
    char *options = options_buf;

    catch = gof.opts;  /* keep live across the whole loop */

    while (*options) {
        unsigned long long bits = 0;

        if (*options != '[') break;

        idx = stridx("-+!", *++options);
        if (idx == -1) {
            printf("FAIL bad group char '%c'\n", *options);
            return 1;
        }

        /* Inner loop: while (*options++ != ']') */
        while (*options++ != ']') {
            struct opts *opt;
            long long ll;

            /* The critical for(;;) loop — exact toybox pattern.
             * gof.opts dereference through struct pointer,
             * two 64-bit vars (ll + bits), opt pointer,
             * idx variable, *options dereference.
             * saveflags, catch, nextarg, ss, rc all live across this loop. */
            for (ll = 1, opt = gof.opts; ; ll <<= 1, opt = opt->next) {
                /* Bounds-check opt to detect corruption */
                if (opt && ((unsigned long)opt < (unsigned long)&nodes[0] ||
                            (unsigned long)opt > (unsigned long)&nodes[NNODES - 1])) {
                    printf("FAIL opt=%p corrupted (ll=0x%llx)\n",
                           (void *)opt, (unsigned long long)ll);
                    return 1;
                }
                if (*options == ']') {
                    if (!opt) break;
                    if (bits & ll) opt->dex[idx] |= bits & ~ll;
                } else {
                    if (*options == 1) break;
                    if (!opt) {
                        printf("FAIL opt=NULL before finding '%c'\n", *options);
                        return 1;
                    }
                    if (opt->c == (127 & *options)) {
                        bits |= ll;
                        break;
                    }
                }
            }
        }
    }

    /* Use all the extra variables to ensure they stay live across the loop.
     * Mimics toybox's get_optflags post-processing. */
    for (catch = gof.opts; catch; catch = catch->next) {
        saveflags &= ~catch->dex[1];
        if (catch->c == *ss) rc = 1;
    }
    if (nextarg != gof.nextarg) saveflags++;

    /* Verify results — use gof.requires to keep it live */
    sink_requires = gof.requires | saveflags;

    /* Check bits correctness through dex values */
    /* B=pos1, C=pos2, Y=pos24, Z=pos25 */
    /* In the ']' pass, each node with bits&ll set should have gotten
     * opt->dex[0] |= bits & ~ll written */
    unsigned long long expect_bits = (1ULL << 1) | (1ULL << 2) | (1ULL << 24) | (1ULL << 25);
    sink_bits = expect_bits;

    /* Verify node B (pos 1) got dex[0] set with bits of C,Y,Z */
    unsigned long long b_dex = nodes[1].dex[0];
    unsigned long long expect_b_dex = expect_bits & ~(1ULL << 1);
    if (b_dex != expect_b_dex) {
        printf("FAIL nodes[1].dex[0]=0x%llx expected=0x%llx\n",
               (unsigned long long)b_dex, (unsigned long long)expect_b_dex);
        return 1;
    }

    sink_dex = b_dex;
    /* Use rc, ss, letters to keep them live through the whole function */
    if (rc && ss != letters[1]) sink_dex++;
    printf("PASS\n");
    return 0;
}
