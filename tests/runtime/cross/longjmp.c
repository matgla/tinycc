/* Force references to setjmp/longjmp runtime helpers. */
typedef unsigned long jmp_buf[8];

extern int setjmp(jmp_buf env);
extern void longjmp(jmp_buf env, int val);

jmp_buf env;

int force_longjmp(int x) {
    if (setjmp(env) == 0) {
        longjmp(env, x + 1);
    }
    return x;
}
