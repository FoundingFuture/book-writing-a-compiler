    .build_version macos, 11, 0
    .text
    .globl _twice
    .p2align 2
_twice:
L_twice.b0:
    lsl x0, x0, #1
    ret
    .section __DATA,__mod_init_func,mod_init_funcs
    .p2align 3
    .quad _anti_rt_init
