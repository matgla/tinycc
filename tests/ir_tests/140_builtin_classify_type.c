#include <stdio.h>

struct S { int x; };
union U { int x; float f; };

int main(void)
{
    int i = 0;
    float f = 0.0f;
    double d = 0.0;
    int *p = &i;
    struct S s;
    union U u;
    int arr[4];
    void (*fp)(void);

    printf("%d\n", __builtin_classify_type(i));     /* 1 - integer */
    printf("%d\n", __builtin_classify_type(f));     /* 8 - real */
    printf("%d\n", __builtin_classify_type(d));     /* 8 - real */
    printf("%d\n", __builtin_classify_type(p));     /* 5 - pointer */
    printf("%d\n", __builtin_classify_type(s));     /* 12 - struct */
    printf("%d\n", __builtin_classify_type(u));     /* 13 - union */
    printf("%d\n", __builtin_classify_type(0));     /* 1 - integer */
    printf("%d\n", __builtin_classify_type(0.0));   /* 8 - real */
    printf("%d\n", __builtin_classify_type((char)0)); /* 1 - integer */
    return 0;
}
