#define PAIR(a, b) a##b
int e1 = PAIR(, 5);
int e2 = PAIR(5, );
int e3 = PAIR(, );

#define TWO(a, b) [a][b]
int e4 TWO(, x);

#define OPEQ(a, b) a ## b
int v = 1;
v OPEQ(+, =) 3;
