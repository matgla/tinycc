/*
 * tu_func_summary: the map from registers to the statics they point at has a fixed
 * size. A function past it lost track of every address it took later, so storing one
 * did not count as an escape, the static was taken to have no readers, and
 * dead_static_store removed the stores to it. Now an overflowed map means anything
 * may have escaped. Seen on the kernel: /tmp's allocator lost its pool pointer once
 * the function setting it up grew past the limit, and /tmp stopped mounting.
 */
#include <stdio.h>

struct pool { int pages; };
struct inner { struct pool *pool; };
struct opt_inner { struct inner payload; _Bool is_null; };
struct vtable;
struct handle { void *self; const struct vtable *vtable; };
struct vtable { int (*pages)(void *); };
struct outer { struct handle first; struct handle second; };
struct opt_outer { struct outer payload; _Bool is_null; };

static struct pool the_pool;
static struct opt_inner the_inner = { { (struct pool *)0xaaaaaaaaul }, 1 };
static struct opt_outer the_outer;

__attribute__((noinline)) static int opaque(int x) { return x * 3; }

static int inner_pages(void *self) { return ((struct inner *)self)->pool->pages; }
static const struct vtable inner_vtable = { inner_pages };
static struct handle inner_handle(struct inner *self)
{
    struct handle handle = { self, &inner_vtable };
    return handle;
}

static int outer_pages(void *self)
{
    struct handle first = ((struct outer *)self)->first;
    return first.vtable->pages(first.self);
}
static const struct vtable outer_vtable = { outer_pages };
static struct handle outer_handle(struct outer *self)
{
    struct handle handle = { self, &outer_vtable };
    return handle;
}

static int total;

static struct handle set_up(int direct, struct handle spare)
{
    struct opt_inner fresh;
    struct inner *address;
    struct handle first;

    total += opaque(0) ^ 1;
    total += opaque(1) ^ 2;
    total += opaque(2) ^ 3;
    total += opaque(3) ^ 4;
    total += opaque(4) ^ 5;
    total += opaque(5) ^ 6;
    total += opaque(6) ^ 7;
    total += opaque(7) ^ 8;
    total += opaque(8) ^ 9;
    total += opaque(9) ^ 10;
    total += opaque(10) ^ 11;
    total += opaque(11) ^ 12;
    total += opaque(12) ^ 13;
    total += opaque(13) ^ 14;
    total += opaque(14) ^ 15;
    total += opaque(15) ^ 16;
    total += opaque(16) ^ 17;
    total += opaque(17) ^ 18;
    total += opaque(18) ^ 19;
    total += opaque(19) ^ 20;
    total += opaque(20) ^ 21;
    total += opaque(21) ^ 22;
    total += opaque(22) ^ 23;
    total += opaque(23) ^ 24;
    total += opaque(24) ^ 25;
    total += opaque(25) ^ 26;
    total += opaque(26) ^ 27;
    total += opaque(27) ^ 28;
    total += opaque(28) ^ 29;
    total += opaque(29) ^ 30;
    total += opaque(30) ^ 31;
    total += opaque(31) ^ 32;
    total += opaque(32) ^ 33;
    total += opaque(33) ^ 34;
    total += opaque(34) ^ 35;
    total += opaque(35) ^ 36;
    total += opaque(36) ^ 37;
    total += opaque(37) ^ 38;
    total += opaque(38) ^ 39;
    total += opaque(39) ^ 40;
    total += opaque(40) ^ 41;
    total += opaque(41) ^ 42;
    total += opaque(42) ^ 43;
    total += opaque(43) ^ 44;
    total += opaque(44) ^ 45;
    total += opaque(45) ^ 46;
    total += opaque(46) ^ 47;
    total += opaque(47) ^ 48;
    total += opaque(48) ^ 49;
    total += opaque(49) ^ 50;
    total += opaque(50) ^ 51;
    total += opaque(51) ^ 52;
    total += opaque(52) ^ 53;
    total += opaque(53) ^ 54;
    total += opaque(54) ^ 55;
    total += opaque(55) ^ 56;
    total += opaque(56) ^ 57;
    total += opaque(57) ^ 58;
    total += opaque(58) ^ 59;
    total += opaque(59) ^ 60;
    total += opaque(60) ^ 61;
    total += opaque(61) ^ 62;
    total += opaque(62) ^ 63;
    total += opaque(63) ^ 64;
    total += opaque(64) ^ 65;
    total += opaque(65) ^ 66;
    total += opaque(66) ^ 67;
    total += opaque(67) ^ 68;
    total += opaque(68) ^ 69;
    total += opaque(69) ^ 70;
    total += opaque(70) ^ 71;
    total += opaque(71) ^ 72;
    total += opaque(72) ^ 73;
    total += opaque(73) ^ 74;
    total += opaque(74) ^ 75;
    total += opaque(75) ^ 76;
    total += opaque(76) ^ 77;
    total += opaque(77) ^ 78;
    total += opaque(78) ^ 79;
    total += opaque(79) ^ 80;
    total += opaque(80) ^ 81;
    total += opaque(81) ^ 82;
    total += opaque(82) ^ 83;
    total += opaque(83) ^ 84;
    total += opaque(84) ^ 85;
    total += opaque(85) ^ 86;
    total += opaque(86) ^ 87;
    total += opaque(87) ^ 88;
    total += opaque(88) ^ 89;
    total += opaque(89) ^ 90;
    total += opaque(90) ^ 91;
    total += opaque(91) ^ 92;
    total += opaque(92) ^ 93;
    total += opaque(93) ^ 94;
    total += opaque(94) ^ 95;
    total += opaque(95) ^ 96;
    total += opaque(96) ^ 97;
    total += opaque(97) ^ 98;
    total += opaque(98) ^ 99;
    total += opaque(99) ^ 100;
    total += opaque(100) ^ 101;
    total += opaque(101) ^ 102;
    total += opaque(102) ^ 103;
    total += opaque(103) ^ 104;
    total += opaque(104) ^ 105;
    total += opaque(105) ^ 106;
    total += opaque(106) ^ 107;
    total += opaque(107) ^ 108;
    total += opaque(108) ^ 109;
    total += opaque(109) ^ 110;
    total += opaque(110) ^ 111;
    total += opaque(111) ^ 112;
    total += opaque(112) ^ 113;
    total += opaque(113) ^ 114;
    total += opaque(114) ^ 115;
    total += opaque(115) ^ 116;
    total += opaque(116) ^ 117;
    total += opaque(117) ^ 118;
    total += opaque(118) ^ 119;
    total += opaque(119) ^ 120;
    total += opaque(120) ^ 121;
    total += opaque(121) ^ 122;
    total += opaque(122) ^ 123;
    total += opaque(123) ^ 124;
    total += opaque(124) ^ 125;
    total += opaque(125) ^ 126;
    total += opaque(126) ^ 127;
    total += opaque(127) ^ 128;
    total += opaque(128) ^ 129;
    total += opaque(129) ^ 130;
    total += opaque(130) ^ 131;
    total += opaque(131) ^ 132;
    total += opaque(132) ^ 133;
    total += opaque(133) ^ 134;
    total += opaque(134) ^ 135;
    total += opaque(135) ^ 136;
    total += opaque(136) ^ 137;
    total += opaque(137) ^ 138;
    total += opaque(138) ^ 139;
    total += opaque(139) ^ 140;
    total += opaque(140) ^ 141;
    total += opaque(141) ^ 142;
    total += opaque(142) ^ 143;
    total += opaque(143) ^ 144;
    total += opaque(144) ^ 145;
    total += opaque(145) ^ 146;
    total += opaque(146) ^ 147;
    total += opaque(147) ^ 148;
    total += opaque(148) ^ 149;
    total += opaque(149) ^ 150;
    total += opaque(150) ^ 151;
    total += opaque(151) ^ 152;
    total += opaque(152) ^ 153;
    total += opaque(153) ^ 154;
    total += opaque(154) ^ 155;
    total += opaque(155) ^ 156;
    total += opaque(156) ^ 157;
    total += opaque(157) ^ 158;
    total += opaque(158) ^ 159;
    total += opaque(159) ^ 160;
    total += opaque(160) ^ 161;
    total += opaque(161) ^ 162;
    total += opaque(162) ^ 163;
    total += opaque(163) ^ 164;
    total += opaque(164) ^ 165;
    total += opaque(165) ^ 166;
    total += opaque(166) ^ 167;
    total += opaque(167) ^ 168;
    total += opaque(168) ^ 169;
    total += opaque(169) ^ 170;
    total += opaque(170) ^ 171;
    total += opaque(171) ^ 172;
    total += opaque(172) ^ 173;
    total += opaque(173) ^ 174;
    total += opaque(174) ^ 175;
    total += opaque(175) ^ 176;
    total += opaque(176) ^ 177;
    total += opaque(177) ^ 178;
    total += opaque(178) ^ 179;
    total += opaque(179) ^ 180;
    total += opaque(180) ^ 181;
    total += opaque(181) ^ 182;
    total += opaque(182) ^ 183;
    total += opaque(183) ^ 184;
    total += opaque(184) ^ 185;
    total += opaque(185) ^ 186;
    total += opaque(186) ^ 187;
    total += opaque(187) ^ 188;
    total += opaque(188) ^ 189;
    total += opaque(189) ^ 190;
    total += opaque(190) ^ 191;
    total += opaque(191) ^ 192;
    total += opaque(192) ^ 193;
    total += opaque(193) ^ 194;
    total += opaque(194) ^ 195;
    total += opaque(195) ^ 196;
    total += opaque(196) ^ 197;
    total += opaque(197) ^ 198;
    total += opaque(198) ^ 199;
    total += opaque(199) ^ 200;

    /* Every register above came first, so what follows is past the map. Written
     * directly, and the address passed along in a handle: nothing reads it by name. */
    the_pool.pages = 100;
    fresh.is_null = 0;
    fresh.payload.pool = &the_pool;
    the_inner = fresh;
    if (!direct) {
        the_outer.is_null = 0;
        address = &the_inner.payload;
        first = inner_handle(address);
        the_outer.payload.first = first;
        the_outer.payload.second = spare;
        return outer_handle(&the_outer.payload);
    }
    address = &the_inner.payload;
    return inner_handle(address);
}

/* Must not change: a static nobody reads at all may still lose its stores. */
static int write_only;

__attribute__((noinline)) static void keep_busy(void)
{
    write_only = 3;
}

int main(int argc, char **argv)
{
    struct handle spare = { 0, 0 };
    struct handle handle = set_up(argc > 100, spare);
    keep_busy();
    total += handle.vtable->pages(handle.self);
    printf("total %d\n", total);
    return 0;
}
