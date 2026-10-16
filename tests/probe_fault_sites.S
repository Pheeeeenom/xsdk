// Instructions that fault, each at a global label, for tests/probe_fault.c. A handler that resumes moves IAR past the
// label's instruction, and the function returns as if nothing happened.

	.set FRAME, 384
	.set SAVE_GPR, 0x50
	.set SAVE_FPR, SAVE_GPR + 18 * 8
	.set SLOT_CR, SAVE_FPR + 18 * 8
	.set AFTER_FPR, 18 * 8
	// The patterns table: a 64-bit value for each of r0-r31, then f14-f31, CTR, CR and XER.
	.set PATTERN_FPR, 32 * 8
	.set PATTERN_CTR, PATTERN_FPR + 18 * 8
	.set PATTERN_CR, PATTERN_CTR + 8
	.set PATTERN_XER, PATTERN_CR + 4

	.section .rodata.probe_fault_patterns, "a"
	.balign 8
	.globl probe_fault_patterns
probe_fault_patterns:
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.long 0xC0DE0000 + \n, 0xFACE0000 + \n
	.endr
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.long 0x3FF0DE00 + \n, 0xBEEF0000 + \n
	.endr
	.long 0xC0DE00C7, 0xFACE00C7
	.long 0x13579BDF
	// SO, CA and a byte count of 0x35: bits XER keeps.
	.long 0xA0000035

	.section .bss.probe_fault_patterned_entry, "aw", @nobits
	.balign 4
	.globl probe_fault_patterned_lr
probe_fault_patterned_lr:
	.space 4
	.globl probe_fault_patterned_r1
probe_fault_patterned_r1:
	.space 4

// Stores r0 to address with every register it can set holding its pattern; after a resume, stores r14-r31 and f14-f31
// as they came back into after[36], then puts them back as they were.
// void probe_fault_patterned_store(uint32_t address, uint64_t *after)
	.section .text.probe_fault_patterned_store, "ax"
	.balign 4
	.globl probe_fault_patterned_store
	.type probe_fault_patterned_store, @function
probe_fault_patterned_store:
	mflr r0
	stw r0, 4(r1)
	stwu r1, -FRAME(r1)
	lis r11, probe_fault_patterned_lr@ha
	stw r0, probe_fault_patterned_lr@l(r11)
	lis r11, probe_fault_patterned_r1@ha
	stw r1, probe_fault_patterned_r1@l(r11)
	mfcr r0
	stw r0, SLOT_CR(r1)
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	std r\n, SAVE_GPR + 8 * (\n - 14)(r1)
	stfd f\n, SAVE_FPR + 8 * (\n - 14)(r1)
	.endr

	lis r12, probe_fault_patterns@ha
	addi r12, r12, probe_fault_patterns@l
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	ld r\n, 8 * \n(r12)
	lfd f\n, PATTERN_FPR + 8 * (\n - 14)(r12)
	.endr
	.irp n, 5,6,7,8,9,10
	ld r\n, 8 * \n(r12)
	.endr
	ld r0, 0(r12)
	ld r11, PATTERN_CTR(r12)
	mtctr r11
	lwz r11, PATTERN_CR(r12)
	mtcrf 0xFF, r11
	lwz r11, PATTERN_XER(r12)
	mtxer r11
	ld r11, 8 * 11(r12)
	ld r12, 8 * 12(r12)
	.globl probe_fault_patterned_site
probe_fault_patterned_site:
	stw r0, 0(r3)

	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	std r\n, 8 * (\n - 14)(r4)
	stfd f\n, AFTER_FPR + 8 * (\n - 14)(r4)
	.endr
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	ld r\n, SAVE_GPR + 8 * (\n - 14)(r1)
	lfd f\n, SAVE_FPR + 8 * (\n - 14)(r1)
	.endr
	lwz r0, SLOT_CR(r1)
	mtcrf 0xFF, r0
	addi r1, r1, FRAME
	lwz r0, 4(r1)
	mtlr r0
	blr
	.size probe_fault_patterned_store, . - probe_fault_patterned_store

// void probe_fault_store(uint32_t address)
	.section .text.probe_fault_store, "ax"
	.balign 4
	.globl probe_fault_store
	.type probe_fault_store, @function
probe_fault_store:
	li r0, 0
	.globl probe_fault_store_site
probe_fault_store_site:
	stw r0, 0(r3)
	blr
	.size probe_fault_store, . - probe_fault_store

// void probe_fault_load(uint32_t address)
	.section .text.probe_fault_load, "ax"
	.balign 4
	.globl probe_fault_load
	.type probe_fault_load, @function
probe_fault_load:
	.globl probe_fault_load_site
probe_fault_load_site:
	lwz r3, 0(r3)
	blr
	.size probe_fault_load, . - probe_fault_load

// A 64-bit store, which faults where address isn't 8-byte aligned in write-combined memory.
// void probe_fault_store_doubleword(uint32_t address)
	.section .text.probe_fault_store_doubleword, "ax"
	.balign 4
	.globl probe_fault_store_doubleword
	.type probe_fault_store_doubleword, @function
probe_fault_store_doubleword:
	li r0, 0
	.globl probe_fault_store_doubleword_site
probe_fault_store_doubleword_site:
	std r0, 0(r3)
	blr
	.size probe_fault_store_doubleword, . - probe_fault_store_doubleword

// Primary opcode 0, which PowerPC leaves undefined.
// void probe_fault_illegal(uint32_t unused)
	.section .text.probe_fault_illegal, "ax"
	.balign 4
	.globl probe_fault_illegal
	.type probe_fault_illegal, @function
probe_fault_illegal:
	.globl probe_fault_illegal_site
probe_fault_illegal_site:
	.long 0
	blr
	.size probe_fault_illegal, . - probe_fault_illegal

// An unconditional trap, as __builtin_trap emits.
// void probe_fault_trap(uint32_t unused)
	.section .text.probe_fault_trap, "ax"
	.balign 4
	.globl probe_fault_trap
	.type probe_fault_trap, @function
probe_fault_trap:
	.globl probe_fault_trap_site
probe_fault_trap_site:
	trap
	blr
	.size probe_fault_trap, . - probe_fault_trap
