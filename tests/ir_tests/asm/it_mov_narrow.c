/* A 0/1 or small constant selected inside an IT block is the 16-bit MOV,
 * which sets no flags there (test_it_mov_narrow). */
int sel(int a, int b) { return a == b ? 5 : 9; }
int both(int a, int b) { return a && b; }
int either(int a, int b) { return a || b; }
