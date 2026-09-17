    .build_version macos, 11, 0
    .text
    .globl _anti.rt.main
    .set _anti.rt.main, _fnptr.main
    .p2align 2
_fnptr.twice:
L_fnptr.twice.b0:
    lsl w0, w0, #1
    ret
    .p2align 2
_fnptr.run:
L_fnptr.run.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldr x9, [x0]
    mov w0, w1
    blr x9
    ldp x29, x30, [sp], #16
    ret
    .p2align 2
_fnptr.main:
L_fnptr.main.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #32
    str x19, [sp, #24]
    str x20, [sp, #16]
    add x19, sp, #0
    adrp x9, _fnptr.twice@PAGE
    add x9, x9, _fnptr.twice@PAGEOFF
    str x9, [x19]
    add x9, x19, #8
    adrp x10, _abs@GOTPAGE
    ldr x10, [x10, _abs@GOTPAGEOFF]
    str x10, [x9]
    add x0, x19, #0
    mov w1, #5
    bl _fnptr.run
    sxtw x20, w0
    add x0, x19, #8
    mov w1, #-3
    bl _fnptr.run
    sxtw x9, w0
    add x0, x20, x9
    ldr x19, [sp, #24]
    ldr x20, [sp, #16]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
