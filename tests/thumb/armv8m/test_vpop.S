.syntax unified
.thumb

.global _start
_start:

.global test_vpop
test_vpop:
    vpop {s0, s1, s2, s3}
    vpop {s4, s5, s6, s7}
    vpop {s8, s9, s10, s11}
    vpop {s12, s13, s14, s15}
    vpop {s16, s17, s18, s19}
    vpop {s20, s21, s22, s23}
    vpop {s24, s25, s26, s27}
    vpop {s29, s30, s31}
    vpop {s0-s31}

    vpop {s0-s11}
    vpop {s20-s31}
    vpop {d0-d15}
    vpop {d0, d1, d2}
    vpop {d8, d9, d10, d11, d12}
    vpop {d14, d15}
    vpop {d0}

