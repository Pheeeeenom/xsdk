// Sets r14-r31 whole, f14-f31 and v14-v31 to known values, calls fn(arg) as the kernel calls a title's code, and
// stores what they hold afterwards.
// void test_entry_regs_call(void (*fn)(void *), void *arg, EntryRegs *out)
//
// Its own slots sit above 0x50(r1): the 64 bytes at 0x10 are the callee's to spill r3-r10 into.

	.set FRAME, 704
	.set SLOT_FN, 0x50
	.set SLOT_ARG, 0x54
	.set SLOT_OUT, 0x58
	.set SLOT_CR, 0x5C
	.set SAVE_GPR, 0x60
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

// Stores r14-r31, f14-f31 and v14-v31 as they are into *out, 16-byte aligned, and changes none of them.
// void test_entry_snapshot(EntryRegs *out)
	.section .text.test_entry_snapshot, "ax"
	.balign 4
	.globl test_entry_snapshot
	.type test_entry_snapshot, @function
test_entry_snapshot:
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	std r\n, 8 * (\n - 14)(r3)
	stfd f\n, OUT_FPR + 8 * (\n - 14)(r3)
	.endr
	addi r11, r3, OUT_VR
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	stvx v\n, 0, r11
	addi r11, r11, 16
	.endr
	blr
	.size test_entry_snapshot, . - test_entry_snapshot

// Calls setjmp(test_jump), then fn, which longjmps back; returns setjmp's second value. It keeps nothing in r14-r31,
// f14-f31 or v14-v31 itself, so what its caller holds there comes back only through setjmp and longjmp.
// int test_run_setjmp(void (*fn)(void))
	.section .text.test_run_setjmp, "ax"
	.balign 4
	.globl test_run_setjmp
	.type test_run_setjmp, @function
test_run_setjmp:
	mflr r0
	stw r0, 4(r1)
	stwu r1, -96(r1)
	stw r3, 0x50(r1)
	lis r3, test_jump@ha
	addi r3, r3, test_jump@l
	bl setjmp
	cmpwi r3, 0
	bne 1f
	lwz r12, 0x50(r1)
	mtctr r12
	bctrl
	li r3, 0
1:	addi r1, r1, 96
	lwz r0, 4(r1)
	mtlr r0
	blr
	.size test_run_setjmp, . - test_run_setjmp
