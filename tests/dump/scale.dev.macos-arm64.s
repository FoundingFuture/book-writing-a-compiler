    .build_version macos, 11, 0
    .text
    .globl _com.example.scale.scale
    .private_extern _com.example.scale.scale
    .p2align 2
_com.example.scale.scale:
L_com.example.scale.scale.b0:
    mov x9, #6
    mul x0, x0, x9
    ret
