.syntax unified
.thumb

.global _start 
_start:

.global test_svc
test_svc:
    svc #0
    svc #0xff
    svc 10
    
    svc.n #0xde

    itte le
    svcle 1
    svcle 2
    svcgt 3
