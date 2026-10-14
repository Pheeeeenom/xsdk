# void lr_hold(uint32_t ticks, const uint64_t *pattern, uint64_t *out)
# Holds *pattern in LR and CTR while spinning until the time base has moved ticks, then stores LR to out[0] and CTR
# to out[1]. The caller's LR is kept on the stack.
	.text
	.globl lr_hold
	.type lr_hold, @function
lr_hold:
	stwu r1, -32(r1)
	mflr r0
	stw r0, 36(r1)
	ld r6, 0(r4)
	mtlr r6
	mtctr r6
	mftb r7
1:	mftb r8
	subf r8, r7, r8
	cmplw r8, r3
	blt 1b
	mflr r8
	std r8, 0(r5)
	mfctr r8
	std r8, 8(r5)
	lwz r0, 36(r1)
	mtlr r0
	addi r1, r1, 32
	blr
	.size lr_hold, .-lr_hold
