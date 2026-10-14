#include <stdio.h>

/* Narrow character constants use the target's char signedness for one byte,
   but multi-character constants concatenate unsigned bytes. */
#if '\377' != 255
#error "unsigned target char must make \\377 equal 255"
#endif
#if 'ab\377' != 0x6162ff
#error "multi-character constants must preserve the high bytes"
#endif

int main(void)
{
    printf("%d %d\n", '\377', 'ab\377');
    return 0;
}
