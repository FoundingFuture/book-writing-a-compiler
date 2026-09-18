    .text
    .globl twice
    .p2align 2
twice:
.Ltwice.b0:
    lsl x0, x0, #1
    ret
    .section .CRT$XCU,"dr"
    .p2align 3
    .quad anti_rt_init
