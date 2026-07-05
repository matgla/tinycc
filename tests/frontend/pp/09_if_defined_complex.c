#define FOO 1
#define BAR 0
#if defined(FOO) && (defined BAR || !defined(BAZ))
int a = 1;
#else
int a = 0;
#endif

#if (defined(FOO) ? BAR : FOO) == 0
int b = 1;
#else
int b = 0;
#endif
