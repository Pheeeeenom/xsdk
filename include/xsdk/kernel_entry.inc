#ifndef XSDK_KERNEL_ENTRY_INC
#define XSDK_KERNEL_ENTRY_INC

// Defines name, called by the kernel, which runs the C function target with the same arguments.
	.macro XSDK_KERNEL_ENTRY name, target
	.section .text.\name, "ax"
	.balign 4
	.globl	\name
	.type	\name, @function
\name:
	mflr	r0
	// The kernel may start r1 at its stack top (xenia:src/xenia/cpu/thread_state.cc:54); GCC writes LR above r1.
	stwu	r1, -32(r1)
	stw	r0, 28(r1)
	bl	\target
	lwz	r0, 28(r1)
	addi	r1, r1, 32
	mtlr	r0
	blr
	.size	\name, . - \name
	.endm

#endif
