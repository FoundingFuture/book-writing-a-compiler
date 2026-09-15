    .build_version macos, 11, 0
    .text
    .globl _anti.rt.main
    .set _anti.rt.main, _strings.main
    .p2align 2
_strings.tail:
L_strings.tail.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #32
    add x9, sp, #0
    str x0, [x9]
    str x1, [x9, #8]
    add x10, sp, #16
    ldr x11, [x9]
    ldr x9, [x9, #8]
    add x11, x11, x2
    str x11, [x10]
    sub x9, x9, x2
    str x9, [x10, #8]
    ldr x0, [x10]
    ldr x1, [x10, #8]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
    .p2align 2
_strings.main:
L_strings.main.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #80
    str x19, [sp, #72]
    str x20, [sp, #64]
    add x19, sp, #0
    add x9, sp, #16
    add x20, sp, #32
    adrp x10, _strings.0@PAGE
    add x10, x10, _strings.0@PAGEOFF
    str x10, [x9]
    mov x10, #5
    str x10, [x9, #8]
    ldr x0, [x9]
    ldr x1, [x9, #8]
    mov x2, #1
    bl _strings.tail
    add x9, sp, #48
    str x0, [x9]
    str x1, [x9, #8]
    ldr x10, [x9]
    str x10, [x19]
    ldr x9, [x9, #8]
    str x9, [x19, #8]
    adrp x9, _strings.1@PAGE
    add x9, x9, _strings.1@PAGEOFF
    str x9, [x20]
    mov x9, #2
    str x9, [x20, #8]
    ldr x0, [x20]
    bl _puts
    ldr x9, [x19]
    ldrb w9, [x9]
    uxtb w0, w9
    ldr x19, [sp, #72]
    ldr x20, [sp, #64]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
    .section __TEXT,__const
_strings.0:
    .byte 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00
_strings.1:
    .byte 0x68, 0x69, 0x00
