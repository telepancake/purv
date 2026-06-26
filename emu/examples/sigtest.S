.section .text
.globl _start
_start:
    la   a0, begin_signature
    # word0 = 6 * 7   (exercises M extension: mul)
    li   t0, 6
    li   t1, 7
    mul  t2, t0, t1
    sw   t2, 0(a0)
    # word1 = 100 + 23
    li   t2, 123
    sw   t2, 4(a0)
    # word2 = 0xdeadbeef
    li   t2, 0xdeadbeef
    sw   t2, 8(a0)
    # word3 = (1 << 20) - 1
    li   t2, 0xfffff
    sw   t2, 12(a0)
    # halt: store nonzero to tohost
    la   t3, tohost
    li   t4, 1
    sw   t4, 0(t3)
1:  j    1b

.section .data
.balign 4
.globl begin_signature
begin_signature:
    .fill 4, 4, 0
.globl end_signature
end_signature:
.balign 8
.globl tohost
tohost:  .word 0
.globl fromhost
fromhost: .word 0
