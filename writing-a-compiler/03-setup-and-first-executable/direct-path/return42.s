    .build_version macos, 11, 0
    .text
    .globl  _anti.rt.main
    .set    _anti.rt.main, _return42.main
    .p2align 2
_return42.main:
    movz    x0, #42
    ret
