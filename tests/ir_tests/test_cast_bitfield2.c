#include <stdio.h>

struct S {
    unsigned ub : 5;
    unsigned u : 32;
    unsigned long long ullb : 35;
    unsigned long long ull : 64;
    char c : 5;
} s;

void promote(int x) {
    printf("   signed : test\n");
}

int main() {
    s.ub = 15;
    s.u = 1;
    s.ullb = 1;
    s.ull = 1;
    s.c = 1;
    
    // These work
    promote(~s.ub);
    promote(~s.u);
    promote(~s.ullb);
    promote(~s.ull);
    promote(~s.c);
    printf("\n");
    
    // This should crash according to the full test
    promote(+(unsigned)s.ub);
    printf("After cast\n");
    
    return 0;
}
