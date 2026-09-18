    .text
    .globl _A4anti2rt_main
    .set _A4anti2rt_main, _A7strings_main
_A7strings_tail:
    .seh_proc _A7strings_tail
.L_A7strings_tail.b0:
    pushq %rbp
    .seh_pushreg %rbp
    movq %rsp, %rbp
    subq $16, %rsp
    .seh_stackalloc 16
    .seh_endprologue
    leaq (%rsp), %rax
    movq (%rdx), %r9
    movq 8(%rdx), %rdx
    addq %r8, %r9
    movq %r9, (%rax)
    subq %r8, %rdx
    movq %rdx, 8(%rax)
    movq (%rax), %rdx
    movq %rdx, (%rcx)
    movq 8(%rax), %rax
    movq %rax, 8(%rcx)
    movq %rcx, %rax
    addq $16, %rsp
    popq %rbp
    ret
    .seh_endproc
_A7strings_main:
    .seh_proc _A7strings_main
.L_A7strings_main.b0:
    pushq %rbp
    .seh_pushreg %rbp
    movq %rsp, %rbp
    subq $112, %rsp
    .seh_stackalloc 112
    movq %rbx, 104(%rsp)
    .seh_savereg %rbx, 104
    movq %rsi, 96(%rsp)
    .seh_savereg %rsi, 96
    .seh_endprologue
    leaq 32(%rsp), %rbx
    leaq 48(%rsp), %rax
    leaq _A7strings_0(%rip), %rcx
    movq %rcx, (%rax)
    movq $5, 8(%rax)
    leaq 64(%rsp), %rsi
    leaq 80(%rsp), %rdx
    movq (%rax), %rcx
    movq %rcx, (%rdx)
    movq 8(%rax), %rax
    movq %rax, 8(%rdx)
    movq $1, %r8
    movq %rsi, %rcx
    call _A7strings_tail
    movq (%rsi), %rax
    movq %rax, (%rbx)
    movq 8(%rsi), %rax
    movq %rax, 8(%rbx)
    leaq _A7strings_1(%rip), %rcx
    call puts
    movq (%rbx), %rax
    movb (%rax), %al
    movzbq %al, %rax
    movq 104(%rsp), %rbx
    movq 96(%rsp), %rsi
    addq $112, %rsp
    popq %rbp
    ret
    .seh_endproc
    .section .rdata,"dr"
_A7strings_0:
    .byte 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00
_A7strings_1:
    .byte 0x68, 0x69, 0x00
