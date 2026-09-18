    .text
    .globl _A4anti2rt_main
    .set _A4anti2rt_main, _A4main_main
_A4main_scale:
.L_A4main_scale.b0:
    imulq $6, %rcx, %rax
    ret
_A4main_main:
    .seh_proc _A4main_main
.L_A4main_main.b0:
    pushq %rbp
    .seh_pushreg %rbp
    movq %rsp, %rbp
    subq $32, %rsp
    .seh_stackalloc 32
    .seh_endprologue
    movq $7, %rcx
    call _A4main_scale
    addq $32, %rsp
    popq %rbp
    ret
    .seh_endproc
