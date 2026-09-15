    .text
    .globl _A4anti2rt_main
    .set _A4anti2rt_main, _A4main_main
    .p2align 2
_A4main_scale:
.L_A4main_scale.b0:
    mov x9, #6
    mul x0, x0, x9
    ret
    .p2align 2
_A4main_main:
    .seh_proc _A4main_main
.L_A4main_main.b0:
    stp x29, x30, [sp, #-16]!
    .seh_save_fplr_x 16
    mov x29, sp
    .seh_set_fp
    .seh_endprologue
    mov x0, #7
    bl _A4main_scale
    .seh_startepilogue
    ldp x29, x30, [sp], #16
    .seh_save_fplr_x 16
    .seh_endepilogue
    ret
    .seh_endproc
