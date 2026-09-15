    .text
    .globl anti.rt.main
    .set anti.rt.main, main.main
main.scale:
.Lmain.scale.b0:
    imulq $6, %rdi, %rax
    ret
main.main:
.Lmain.main.b0:
    pushq %rbp
    movq %rsp, %rbp
    movq $7, %rdi
    call main.scale
    popq %rbp
    ret
    .section .note.GNU-stack,"",@progbits
