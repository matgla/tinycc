#define ADD3(a, b, c) \
    ((a) + /* first add */ \
     (b) + \
     (c)) /* final */
int total = ADD3(1, 2, 3);

#define COMMENTED_VALUE /* leading comment */ 99 /* trailing comment */
int val = COMMENTED_VALUE;
