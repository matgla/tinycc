/* Bit offsets of a member past 256 MB overflowed `int` in struct_layout. */
struct S { char a[0x32100000]; int x:30, y:30; };
_Static_assert(sizeof(struct S) == 0x32100008, "size");
int gety(struct S *s) { return s->y; }
