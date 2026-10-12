#include "xsdk/kernel_entry.inc"

// test_kernel_entry.c's GCC code, called as the kernel calls a title's.
	XSDK_KERNEL_ENTRY test_entry_wrapped, test_entry_target

// Sets r14-r31 whole, f14-f31 and v14-v31 to known values, calls fn(arg), and stores what they hold afterwards.
// void test_entry_regs_call(void (*fn)(void *), void *arg, EntryRegs *out)

	.set FRAME, 704
	.set SLOT_FN, 16
	.set SLOT_ARG, 20
	.set SLOT_OUT, 24
	.set SLOT_CR, 28
	.set SAVE_GPR, 48
	.set SAVE_FPR, SAVE_GPR + 18 * 8
	.set SAVE_VR, SAVE_FPR + 18 * 8 + 16
	.set OUT_FPR, 18 * 8
	.set OUT_VR, OUT_FPR + 18 * 8

	.section .rodata.test_entry_sentinels, "a"
	.balign 16
	.globl test_entry_gpr_sentinels
test_entry_gpr_sentinels:
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.long 0x5A000000 + \n, 0xC3000000 + \n
	.endr
	.globl test_entry_fpr_sentinels
test_entry_fpr_sentinels:
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.long 0x3FF00000 + \n, 0x12340000 + \n
	.endr
	.globl test_entry_vr_sentinels
test_entry_vr_sentinels:
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.long 0x71000000 + \n, 0x72000000 + \n, 0x73000000 + \n, 0x74000000 + \n
	.endr

	.section .text.test_entry_regs_call, "ax"
	.balign 4
	.globl test_entry_regs_call
	.type test_entry_regs_call, @function
test_entry_regs_call:
	mflr r0
	stw r0, 4(r1)
	stwu r1, -FRAME(r1)
	stw r3, SLOT_FN(r1)
	stw r4, SLOT_ARG(r1)
	stw r5, SLOT_OUT(r1)
	mfcr r0
	stw r0, SLOT_CR(r1)
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	std r\n, SAVE_GPR + 8 * (\n - 14)(r1)
	stfd f\n, SAVE_FPR + 8 * (\n - 14)(r1)
	.endr
	addi r11, r1, SAVE_VR
	clrrwi r11, r11, 4
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	stvx v\n, 0, r11
	addi r11, r11, 16
	.endr

	lis r11, test_entry_gpr_sentinels@ha
	addi r11, r11, test_entry_gpr_sentinels@l
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	lfd f\n, OUT_FPR + 8 * (\n - 14)(r11)
	.endr
	addi r12, r11, OUT_VR
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	lvx v\n, 0, r12
	addi r12, r12, 16
	.endr
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	ld r\n, 8 * (\n - 14)(r11)
	.endr

	lwz r12, SLOT_FN(r1)
	mtctr r12
	lwz r3, SLOT_ARG(r1)
	bctrl

	lwz r12, SLOT_OUT(r1)
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	std r\n, 8 * (\n - 14)(r12)
	stfd f\n, OUT_FPR + 8 * (\n - 14)(r12)
	.endr
	addi r11, r12, OUT_VR
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	stvx v\n, 0, r11
	addi r11, r11, 16
	.endr

	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	ld r\n, SAVE_GPR + 8 * (\n - 14)(r1)
	lfd f\n, SAVE_FPR + 8 * (\n - 14)(r1)
	.endr
	addi r11, r1, SAVE_VR
	clrrwi r11, r11, 4
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	lvx v\n, 0, r11
	addi r11, r11, 16
	.endr
	lwz r0, SLOT_CR(r1)
	mtcrf 0xFF, r0
	addi r1, r1, FRAME
	lwz r0, 4(r1)
	mtlr r0
	blr
	.size test_entry_regs_call, . - test_entry_regs_call
