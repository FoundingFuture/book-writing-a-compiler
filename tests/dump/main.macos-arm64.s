    .build_version macos, 11, 0
    .text
    .globl _anti.rt.main
    .set _anti.rt.main, _main.main
    .p2align 2
_main.scale:
L_main.scale.b0:
    mov x9, #6
    mul x0, x0, x9
    ret
    .p2align 2
_main.main:
L_main.main.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x0, #7
    bl _main.scale
    ldp x29, x30, [sp], #16
    ret
