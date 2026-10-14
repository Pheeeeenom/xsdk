# uint32_t regs_hold2(uint32_t ticks, const uint64_t *patterns, uint64_t *out)
# Holds patterns[0..6] in r0, r3, r4, r5, r10, r11, r12, patterns[7] in CTR, the low word of patterns[8] in CR
# (CR0 is the loop's) and of patterns[9] in XER, spinning on the time base with r6-r9; then stores them to out[0..9].
	.text
	.globl regs_hold2
	.type regs_hold2, @function
regs_hold2:
	stwu r1, -64(r1)
	stw r5, 8(r1)              # out
	mr r6, r3                  # ticks
	ld r9, 56(r4)
	mtctr r9
	ld r9, 64(r4)
	mtcrf 0xFF, r9
	ld r9, 72(r4)
	mtxer r9
	mr r9, r4
	ld r0, 0(r9)
	ld r3, 8(r9)
	ld r4, 16(r9)
	ld r5, 24(r9)
	ld r10, 32(r9)
	ld r11, 40(r9)
	ld r12, 48(r9)
	mftb r7
1:	mftb r8
	subf r8, r7, r8
	cmplw r8, r6
	blt 1b
	lwz r9, 8(r1)
	std r0, 0(r9)
	std r3, 8(r9)
	std r4, 16(r9)
	std r5, 24(r9)
	std r10, 32(r9)
	std r11, 40(r9)
	std r12, 48(r9)
	mfctr r8
	std r8, 56(r9)
	mfcr r8
	std r8, 64(r9)
	mfxer r8
	std r8, 72(r9)
	addi r1, r1, 64
	blr
	.size regs_hold2, .-regs_hold2
