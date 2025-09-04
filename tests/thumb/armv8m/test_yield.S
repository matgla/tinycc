.syntax unified
.thumb

.global _start
_start:

.global test_yield
test_yield:
    yield
    yield.w
