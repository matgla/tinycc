/*
 * Bug: GNU ?: (Elvis operator) extension miscompiled.
 *
 * The GNU C extension allows omitting the middle operand in a ternary:
 *   x = a ?: b;   // equivalent to: x = a ? a : b;
 *
 * TCC miscompiles this in context matching toybox's cp command:
 *   - 'tt' derived from a ternary on a global struct pointer dereference
 *   - false branch indexes a global struct's array via pre-decrement
 *
 * This broke toybox's cp command where:
 *   char *tt = *toys.which->name == 'i' ? TT.i.t : TT.c.t;
 *   char *destname = tt ?: toys.optargs[--toys.optc];
 * always picked optargs[0] (source) instead of optargs[optc-1] (dest).
 *
 * Expected: all lines print "PASS"
 */
#include <stdio.h>
#include <string.h>

/* ---- Mimic toybox global structs ---- */

struct cmd_list {
    char *name;
    void (*main_fn)(void);
    char *options;
    unsigned flags;
};

struct toy_ctx {
    struct cmd_list *which;
    char **argv;
    char **optargs;
    unsigned long long optflags;
    int optc;
    short count;
    char exitval;
};

struct cp_data {
    union {
        struct { char *g, *o, *m, *t; } i;  /* install */
        struct { char *t, *preserve; } c;    /* cp */
    };
    char *destname;
};

union global_u {
    struct cp_data cp;
};

/* Globals, like toybox */
struct toy_ctx toys;
union global_u this_union;

#define TT this_union.cp

/* ---- Tests ---- */

/* Test 1: simple local-var elvis (baseline) */
void test1_local_elvis(void)
{
    const char *tt = NULL;
    const char *args[] = {"src.txt", "dst.txt"};
    int optc = 2;
    const char *dest = tt ?: args[--optc];
    if (strcmp(dest, "dst.txt") == 0 && optc == 1)
        printf("test1: PASS\n");
    else
        printf("test1: FAIL dest='%s' optc=%d\n", dest, optc);
}

/* Test 2: global struct elvis - tt is NULL, should pick optargs[--optc] */
void test2_global_null(void)
{
    char *args[] = {"src.txt", "dst.txt"};
    toys.optargs = args;
    toys.optc = 2;
    TT.c.t = NULL;

    struct cmd_list cp_cmd = { "cp", NULL, NULL, 0 };
    toys.which = &cp_cmd;

    char *tt = *toys.which->name == 'i' ? TT.i.t : TT.c.t;
    char *destname = tt ?: toys.optargs[--toys.optc];

    if (strcmp(destname, "dst.txt") == 0 && toys.optc == 1)
        printf("test2: PASS\n");
    else
        printf("test2: FAIL dest='%s' optc=%d\n", destname, toys.optc);
}

/* Test 3: global struct elvis - tt is non-NULL, should pick tt */
void test3_global_nonnull(void)
{
    char *args[] = {"src.txt", "dst.txt"};
    toys.optargs = args;
    toys.optc = 2;
    TT.c.t = "/target/dir";

    struct cmd_list cp_cmd = { "cp", NULL, NULL, 0 };
    toys.which = &cp_cmd;

    char *tt = *toys.which->name == 'i' ? TT.i.t : TT.c.t;
    char *destname = tt ?: toys.optargs[--toys.optc];

    if (strcmp(destname, "/target/dir") == 0 && toys.optc == 2)
        printf("test3: PASS\n");
    else
        printf("test3: FAIL dest='%s' optc=%d\n", destname, toys.optc);
}

/* Test 4: 'install' path - tt via i.t (different union offset) */
void test4_install_null(void)
{
    char *args[] = {"src.txt", "dst.txt"};
    toys.optargs = args;
    toys.optc = 2;
    /* Clear the union */
    memset(&TT, 0, sizeof(TT));

    struct cmd_list inst_cmd = { "install", NULL, NULL, 0 };
    toys.which = &inst_cmd;

    char *tt = *toys.which->name == 'i' ? TT.i.t : TT.c.t;
    char *destname = tt ?: toys.optargs[--toys.optc];

    if (strcmp(destname, "dst.txt") == 0 && toys.optc == 1)
        printf("test4: PASS\n");
    else
        printf("test4: FAIL dest='%s' optc=%d\n", destname, toys.optc);
}

/* Test 5: 3 args - should pick last arg as dest */
void test5_three_args(void)
{
    char *args[] = {"a.txt", "b.txt", "dest_dir"};
    toys.optargs = args;
    toys.optc = 3;
    TT.c.t = NULL;

    struct cmd_list cp_cmd = { "cp", NULL, NULL, 0 };
    toys.which = &cp_cmd;

    char *tt = *toys.which->name == 'i' ? TT.i.t : TT.c.t;
    char *destname = tt ?: toys.optargs[--toys.optc];

    if (strcmp(destname, "dest_dir") == 0 && toys.optc == 2)
        printf("test5: PASS\n");
    else
        printf("test5: FAIL dest='%s' optc=%d\n", destname, toys.optc);
}

/* Test 6: result used immediately in another expression */
void test6_used_in_call(void)
{
    char *args[] = {"src.txt", "dst.txt"};
    toys.optargs = args;
    toys.optc = 2;
    TT.c.t = NULL;

    struct cmd_list cp_cmd = { "cp", NULL, NULL, 0 };
    toys.which = &cp_cmd;

    char *tt = *toys.which->name == 'i' ? TT.i.t : TT.c.t;
    int len = strlen(tt ?: toys.optargs[--toys.optc]);

    if (len == 7 && toys.optc == 1)  /* strlen("dst.txt") == 7 */
        printf("test6: PASS\n");
    else
        printf("test6: FAIL len=%d optc=%d\n", len, toys.optc);
}

/* Test 7: chained elvis with globals */
void test7_chained(void)
{
    const char *a = NULL;
    const char *b = NULL;
    const char *c = "final";
    const char *r = a ?: b ?: c;
    if (strcmp(r, "final") == 0)
        printf("test7: PASS\n");
    else
        printf("test7: FAIL got='%s'\n", r);
}

/* Test 8: elvis inside a loop (register pressure) */
void test8_loop(void)
{
    char *names[] = {"alpha", "beta", "gamma"};
    char *fallback = "NONE";
    char *ptrs[] = { NULL, names[1], NULL };
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        char *r = ptrs[i] ?: fallback;
        if (i == 0 && strcmp(r, "NONE") != 0) ok = 0;
        if (i == 1 && strcmp(r, "beta") != 0) ok = 0;
        if (i == 2 && strcmp(r, "NONE") != 0) ok = 0;
    }
    printf("test8: %s\n", ok ? "PASS" : "FAIL");
}

int main(void)
{
    test1_local_elvis();
    test2_global_null();
    test3_global_nonnull();
    test4_install_null();
    test5_three_args();
    test6_used_in_call();
    test7_chained();
    test8_loop();
    return 0;
}
