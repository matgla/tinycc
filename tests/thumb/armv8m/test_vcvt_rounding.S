.syntax unified
.thumb

.global _start
_start:

.global test_vcvt_rounding
test_vcvt_rounding:
    vcvt.s32.f32 s0, s1
    vcvt.u32.f32 s2, s3
    vcvtr.s32.f32 s0, s1
    vcvtr.u32.f32 s2, s3
    vcvtr.s32.f64 s0, d1
    vcvta.s32.f32 s0, s1
    vcvta.u32.f32 s2, s3
    vcvtn.s32.f32 s0, s1
    vcvtn.u32.f32 s0, s1
    vcvtp.s32.f32 s0, s1
    vcvtm.s32.f32 s0, s1
    vcvtm.u32.f64 s4, d5
    vcvta.s32.f64 s0, d1
    bx lr
