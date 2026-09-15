    .text
    .globl anti.rt.main
    .set anti.rt.main, strings.main
strings.tail:
.Lstrings.tail.b0:
    pushq %rbp
    movq %rsp, %rbp
    subq $32, %rsp
    leaq (%rsp), %rax
    movq %rdi, (%rax)
    movq %rsi, 8(%rax)
    leaq 16(%rsp), %rcx
    movq (%rax), %rsi
    movq 8(%rax), %rax
    addq %rdx, %rsi
    movq %rsi, (%rcx)
    subq %rdx, %rax
    movq %rax, 8(%rcx)
    movq (%rcx), %rax
    movq 8(%rcx), %rdx
    movq %rbp, %rsp
    popq %rbp
    ret
strings.main:
.Lstrings.main.b0:
    pushq %rbp
    movq %rsp, %rbp
    subq $80, %rsp
    movq %rbx, 72(%rsp)
    movq %r12, 64(%rsp)
    leaq (%rsp), %rbx
    leaq 16(%rsp), %rax
    leaq 32(%rsp), %r12
    leaq strings.0(%rip), %rcx
    movq %rcx, (%rax)
    movq $5, 8(%rax)
    movq (%rax), %rdi
    movq 8(%rax), %rsi
    movq $1, %rdx
    call strings.tail
    leaq 48(%rsp), %rcx
    movq %rax, (%rcx)
    movq %rdx, 8(%rcx)
    movq (%rcx), %rax
    movq %rax, (%rbx)
    movq 8(%rcx), %rax
    movq %rax, 8(%rbx)
    leaq strings.1(%rip), %rax
    movq %rax, (%r12)
    movq $2, 8(%r12)
    movq (%r12), %rdi
    call puts
    movq (%rbx), %rax
    movb (%rax), %al
    movzbq %al, %rax
    movq 72(%rsp), %rbx
    movq 64(%rsp), %r12
    movq %rbp, %rsp
    popq %rbp
    ret
    .section .rodata
strings.0:
    .byte 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00
strings.1:
    .byte 0x68, 0x69, 0x00
    .section .note.GNU-stack,"",@progbits
