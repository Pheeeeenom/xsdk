#ifndef XSDK_KERNEL_ENTRY_INC
#define XSDK_KERNEL_ENTRY_INC
// Defines name, called by the kernel, which runs the C function target with the same arguments.
	.set	XSDK_ENTRY_FRAME, 176
	.set	XSDK_ENTRY_GPRS, 16
	.set	XSDK_ENTRY_LR, XSDK_ENTRY_FRAME - 4
	.macro XSDK_KERNEL_ENTRY name, target
	.section .text.\name, "ax"
	.balign 4
	.globl	\name
	.type	\name, @function
\name:
	mflr	r0
	// The kernel may start r1 at its stack top (xenia:src/xenia/cpu/thread_state.cc:54); GCC writes LR above r1.
	stwu	r1, -XSDK_ENTRY_FRAME(r1)
	stw	r0, XSDK_ENTRY_LR(r1)
	// The kernel's code keeps r14-r31 whole across calls, GCC's only their low halves (docs/HARDWARE.md).
	.irp	n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	std	r\n, XSDK_ENTRY_GPRS + 8 * (\n - 14)(r1)
	.endr
	bl	\target
	.irp	n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	ld	r\n, XSDK_ENTRY_GPRS + 8 * (\n - 14)(r1)
	.endr
	lwz	r0, XSDK_ENTRY_LR(r1)
	addi	r1, r1, XSDK_ENTRY_FRAME
	mtlr	r0
	blr
	.size	\name, . - \name
	.endm
#endif
