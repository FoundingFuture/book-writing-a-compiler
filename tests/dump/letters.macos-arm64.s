    .build_version macos, 11, 0
    .text
    .globl _anti.rt.main
    .set _anti.rt.main, _letters.main
    .p2align 2
_letters.main:
L_letters.main.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #16
    str x19, [sp, #8]
    mov x19, #0
L_letters.main.b1:
    cmp x19, #3
    b.ge L_letters.main.b3
L_letters.main.b2:
    mov w9, w19
    add w0, w9, #65
    bl _putchar
    add x19, x19, #1
    b L_letters.main.b1
L_letters.main.b3:
    mov x0, x19
    ldr x19, [sp, #8]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
