
#ifdef __APPLE__
#define CO_SYMBOL(name) _##name
#else
#define CO_SYMBOL(name) name
#endif

.text
.p2align 2
.globl CO_SYMBOL(coctx_swap)
#if !defined( __APPLE__ )
.type  coctx_swap, @function
#endif
CO_SYMBOL(coctx_swap):

#if defined(__i386__)
    movl 4(%esp), %eax
    movl %esp,  28(%eax)
    movl %ebp, 24(%eax)
    movl %esi, 20(%eax)
    movl %edi, 16(%eax)
    movl %edx, 12(%eax)
    movl %ecx, 8(%eax)
    movl %ebx, 4(%eax)


    movl 8(%esp), %eax
    movl 4(%eax), %ebx
    movl 8(%eax), %ecx
    movl 12(%eax), %edx
    movl 16(%eax), %edi
    movl 20(%eax), %esi
    movl 24(%eax), %ebp
    movl 28(%eax), %esp

	ret

#elif defined(__x86_64__)
	leaq (%rsp),%rax
    movq %rax, 104(%rdi)
    movq %rbx, 96(%rdi)
    movq %rcx, 88(%rdi)
    movq %rdx, 80(%rdi)
	  movq 0(%rax), %rax
	  movq %rax, 72(%rdi) 
    movq %rsi, 64(%rdi)
	  movq %rdi, 56(%rdi)
    movq %rbp, 48(%rdi)
    movq %r8, 40(%rdi)
    movq %r9, 32(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 16(%rdi)
    movq %r14, 8(%rdi)
    movq %r15, (%rdi)
	  xorq %rax, %rax

    movq 48(%rsi), %rbp
    movq 104(%rsi), %rsp
    movq (%rsi), %r15
    movq 8(%rsi), %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %r9
    movq 40(%rsi), %r8
    movq 56(%rsi), %rdi
    movq 80(%rsi), %rdx
    movq 88(%rsi), %rcx
    movq 96(%rsi), %rbx
		leaq 8(%rsp), %rsp
		pushq 72(%rsi)

    movq 64(%rsi), %rsi
	ret
#elif defined(__aarch64__)
    // x18 belongs to the platform. Never touch it or migrate thread state.
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    stp d8, d9, [x0, #112]
    stp d10, d11, [x0, #128]
    stp d12, d13, [x0, #144]
    stp d14, d15, [x0, #160]
    mrs x9, fpcr
    mrs x10, fpsr
    stp w9, w10, [x0, #176]

    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]
    ldp d8, d9, [x1, #112]
    ldp d10, d11, [x1, #128]
    ldp d12, d13, [x1, #144]
    ldp d14, d15, [x1, #160]
    ldp w9, w10, [x1, #176]
    msr fpcr, x9
    msr fpsr, x10
    ldr x9, [x1, #96]
    mov sp, x9
    ret

.p2align 2
.globl CO_SYMBOL(coctx_entry)
#ifndef __APPLE__
.type coctx_entry, @function
#endif
CO_SYMBOL(coctx_entry):
    // Establish a terminating frame record for Apple's frame-chain unwinder.
    stp xzr, xzr, [sp, #-16]!
    mov x29, sp
    mov x0, x20
    mov x1, x21
    blr x19
    // CoRoutineFunc yields on completion; a raw entry must not return.
    bl CO_SYMBOL(abort)
    brk #0
#else
#error "Unsupported libco context architecture"
#endif

#if defined(__linux__)
.section .note.GNU-stack,"",@progbits
#endif
