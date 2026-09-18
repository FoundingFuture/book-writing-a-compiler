    .text
    .globl anti.rt.main
    .set anti.rt.main, main.main
    .p2align 2
main.scale:
.Lmain.scale.b0:
    mov x9, #6
    mul x0, x0, x9
    ret
    .p2align 2
main.main:
.Lmain.main.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x0, #7
    bl main.scale
    ldp x29, x30, [sp], #16
    ret
    .section .note.GNU-stack,"",@progbits
