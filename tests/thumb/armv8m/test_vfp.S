.syntax unified
.thumb

.global _start
_start:

.global test_vfp
test_vfp:
    /* Arithmetic SP */
    vadd.f32 s0, s1, s2
    vsub.f32 s0, s1, s2
    vmul.f32 s0, s1, s2
    vdiv.f32 s0, s1, s2
    vneg.f32 s0, s1
    vcmp.f32 s0, s1

    /* Arithmetic DP */
    vadd.f64 d0, d1, d2
    vsub.f64 d0, d1, d2
    vmul.f64 d0, d1, d2
    vdiv.f64 d0, d1, d2
    vneg.f64 d0, d1
    vcmp.f64 d0, d1

    /* Register moves */
    vmov.f32 s1, s2
    vmov.f64 d0, d1

    /* System */
    vmrs r0, fpscr

    /* Conversions SP <-> DP */
    vcvt.f64.f32 d0, s0
    vcvt.f32.f64 s0, d0

    /* Push / Pop SP */
    vpush {s0-s3}
    vpush {s16-s19}
    vpop {s0-s3}
    vpop {s16-s19}

    /* Push / Pop DP */
    vpush {d0-d3}
    vpush {d8-d15}
    vpop {d0-d3}
    vpop {d8-d15}
