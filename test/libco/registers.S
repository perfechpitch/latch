#if defined(__aarch64__)
#ifdef __APPLE__
#define SYMBOL(name) _##name
#else
#define SYMBOL(name) name
#endif
.text
.p2align 2
.globl SYMBOL(co_test_registers)
SYMBOL(co_test_registers):
    stp x29, x30, [sp, #-192]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    str x0, [sp, #160]
    mov x19, x0
    mov x20, x0
    mov x21, x0
    mov x22, x0
    mov x23, x0
    mov x24, x0
    mov x25, x0
    mov x26, x0
    mov x27, x0
    mov x28, x0
    fmov d8, x0
    fmov d9, x0
    fmov d10, x0
    fmov d11, x0
    fmov d12, x0
    fmov d13, x0
    fmov d14, x0
    fmov d15, x0
    bl SYMBOL(co_test_yield)
    ldr x9, [sp, #160]
    mov x0, #1
    mov x10, sp
    cmp x29, x10
    b.ne 1f
.macro check_gpr reg
    cmp \reg, x9
    b.ne 1f
.endm
    check_gpr x19
    check_gpr x20
    check_gpr x21
    check_gpr x22
    check_gpr x23
    check_gpr x24
    check_gpr x25
    check_gpr x26
    check_gpr x27
    check_gpr x28
.macro check_fp reg
    fmov x10, \reg
    cmp x10, x9
    b.ne 1f
.endm
    check_fp d8
    check_fp d9
    check_fp d10
    check_fp d11
    check_fp d12
    check_fp d13
    check_fp d14
    check_fp d15
    mov x0, #0
1:
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x29, x30, [sp], #192
    ret
#endif
#if defined(__linux__)
.section .note.GNU-stack,"",@progbits
#endif
