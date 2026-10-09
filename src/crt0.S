// The kernel enters with r1 at the very top of the stack (xenia:src/xenia/cpu/thread_state.cc:54).
	.section .text._start, "ax"
	.balign 4
	.globl	_start
	.type	_start, @function
_start:
	mflr	r0
	stwu	r1, -32(r1)
	stw	r0, 28(r1)
	bl	xsdk_start
	lwz	r0, 28(r1)
	addi	r1, r1, 32
	mtlr	r0
	blr
	.size	_start, . - _start
