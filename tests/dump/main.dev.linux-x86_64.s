    .text
    .globl anti.rt.main
    .set anti.rt.main, main.main
    .globl main.main
    .hidden main.main
main.main:
.Lmain.main.b0:
    pushq %rbp
    movq %rsp, %rbp
    movq $7, %rdi
    call com.example.twice.twice
    cqto
    movq $2, %rcx
    idivq %rcx
    popq %rbp
    ret
    .section .note.GNU-stack,"",@progbits
