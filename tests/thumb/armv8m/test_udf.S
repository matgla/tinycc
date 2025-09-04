.syntax unified
.thumb

.global _start
_start:

.global test_udf
test_udf:
    udf #0
    udf #0xff
    udf.w #12
    udf.w #0x2234