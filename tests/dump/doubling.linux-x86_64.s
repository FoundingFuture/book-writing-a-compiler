    .text
    .globl twice
twice:
.Ltwice.b0:
    movq %rdi, %rax
    shlq $1, %rax
    ret
    .section .note.GNU-stack,"",@progbits
    .section .init_array,"aw"
    .p2align 3
    .quad anti_rt_init
