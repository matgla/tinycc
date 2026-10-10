extern int g2val;
int helper(int);
int used_var = 7;
void _start(void) { used_var = helper(g2val); }
