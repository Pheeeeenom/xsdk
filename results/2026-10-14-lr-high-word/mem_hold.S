# uint32_t mem_hold(uint32_t ticks, uint32_t *changed)
# Makes a 512-byte frame, fills its doublewords from 8(r1) to 504(r1) with 0xC0DE0000_0000<offset>, spins until the
# time base has moved ticks, then writes each offset whose doubleword changed, and its low word, into changed[] (up
# to 64 pairs) and returns how many changed.
	.text
	.globl mem_hold
	.type mem_hold, @function
mem_hold:
	stwu r1, -512(r1)
	lis r6, 0xC0DE
	sldi r6, r6, 32            # 0xC0DE0000_00000000
	li r5, 8
1:	or r7, r6, r5
	stdx r7, r1, r5
	addi r5, r5, 8
	cmpwi r5, 512
	blt 1b
	mr r0, r3
	mftb r8
2:	mftb r9
	subf r9, r8, r9
	cmplw r9, r0
	blt 2b
	li r3, 0
	li r5, 8
3:	ldx r7, r1, r5
	or r9, r6, r5
	cmpd r7, r9
	beq 4f
	cmpwi r3, 64
	bge 4f
	slwi r10, r3, 3
	stwx r5, r4, r10
	addi r10, r10, 4
	stwx r7, r4, r10
	addi r3, r3, 1
4:	addi r5, r5, 8
	cmpwi r5, 512
	blt 3b
	addi r1, r1, 512
	blr
	.size mem_hold, .-mem_hold
