#define X 3

#if X == 1
int v = 1;
#elif X == 2
int v = 2;
#elif X == 3
int v = 3;
#elif X == 4
int v = 4;
#else
int v = -1;
#endif

#if -1 > 0U
int neg_vs_unsigned = 1;
#else
int neg_vs_unsigned = 0;
#endif

#if 0xFFFFFFFFU == -1
int allones = 1;
#else
int allones = 0;
#endif
