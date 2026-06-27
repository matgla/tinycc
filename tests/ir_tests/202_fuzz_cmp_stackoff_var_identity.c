#include <stdio.h>

int main(void)
{
    unsigned cs = 0x12345678u;
    char s2 = (char)(1008662799u & 0xff);
    unsigned u6 = 3475697136u;
    unsigned u9 = 3964761052u;
    unsigned arr10[8] = {
        1745973260u, 2601192460u, 699164184u, 2787493415u,
        887579110u, 4191126204u, 1727182512u, 3955878842u
    };
    unsigned arr11[8] = {
        2111690304u, 3017749135u, 456660453u, 3723260400u,
        558401104u, 3032161576u, 2522709933u, 51304630u
    };

    if (arr10[0] == 0)
        arr11[0] = 0;

    {
        unsigned a0 = (unsigned)(~((unsigned)(u6 << (1379155468u & 31u)) | 0u));
        unsigned b0 = (unsigned)((unsigned)(-((unsigned)s2 | 0u)) <<
                                 (u9 & 31u)) ^ cs;
        unsigned c0 = (unsigned)(a0 >= b0);
        unsigned v0 = c0 | (unsigned)(~(870061177u | 0u));
        (void)a0;
        (void)b0;
        (void)c0;
        printf("checksum=%08x\n", v0);
    }

    return 0;
}
