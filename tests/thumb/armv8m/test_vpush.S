.syntax unified
.thumb

.global _start
_start:

.global test_vpush
test_vpush:
    vpush {s0, s1, s2, s3}
    vpush {s4, s5, s6, s7}
    vpush {s8, s9, s10, s11}
    vpush {s12, s13, s14, s15}
    vpush {s16, s17, s18, s19}
    vpush {s20, s21, s22, s23}
    vpush {s24, s25, s26, s27}
    vpush {s29, s30, s31}
    vpush {s0-s31}

    vpush {s0-s11}
    vpush {s20-s31}
    vpush {d0-d15}
    vpush {d0, d1, d2}
    vpush {d8, d9, d10, d11, d12}
    vpush {d14, d15}
    vpush {d0}
