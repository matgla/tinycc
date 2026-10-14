.syntax unified
.thumb

.global _start
_start:
.thumb_func
back:
    bx lr
    .space 5000
.thumb_func
f:
    bl back
    b.w back
    nop
    .space 20000
.thumb_func
g:
    bl back
    b.w back
    bl f
    b.w f
    bx lr
