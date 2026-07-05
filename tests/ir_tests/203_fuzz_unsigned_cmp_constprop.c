#include <stdio.h>

int main(void)
{
    unsigned cs = 0x12345678u;
    char s2 = (char)(1008662799u & 0xff);
    unsigned u6 = 3475697136u;
    unsigned u9 = 3964761052u;
    unsigned v = (unsigned)(((unsigned)(((unsigned)((~((unsigned)(((unsigned)(u6) <<
        ((unsigned)(1379155468u) & 31u))) | 0u))) >= ((unsigned)(((unsigned)
        ((-((unsigned)((unsigned)(s2)) | 0u))) << ((unsigned)(u9) & 31u))) ^
        cs))) | (unsigned)((~((unsigned)(870061177u) | 0u)))));

    printf("checksum=%08x\n", v);
    return 0;
}
