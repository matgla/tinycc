#define LOG0(fmt, ...) printf(fmt, ##__VA_ARGS__)
LOG0("no args");
LOG0("one arg: %d", 7);

#define COUNT(...) VA_COUNT(__VA_ARGS__, 5, 4, 3, 2, 1)
#define VA_COUNT(a1, a2, a3, a4, a5, N, ...) N
int n1 = COUNT(a);
int n2 = COUNT(a, b, c);

#define TRAIL(a, ...) a, __VA_ARGS__
int arr[] = { TRAIL(1, 2, 3,) };
