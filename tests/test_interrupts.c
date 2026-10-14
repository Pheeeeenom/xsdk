#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tests.h"
#include "xsdk/log.h"

// The console's kernel keeps only LR's low word across an interrupt (docs/HARDWARE.md), so a 64-bit value GCC parked
// in LR came back with its high word 0 whenever the clock interrupt landed in between. This runs work that keeps more
// 64-bit values live than the GPRs hold, Keccak-f[1600] (FIPS 202, section 3), through half a second of clock
// interrupts, and checks every result.

enum {
	LANES = 25,
	ROUNDS = 24,
	RUN_TICKS = 25000000,
};

// FIPS 202, section 3.2.5's round constants.
static const uint64_t round_constants[ROUNDS] = {
	0x0000000000000001ull,
	0x0000000000008082ull,
	0x800000000000808Aull,
	0x8000000080008000ull,
	0x000000000000808Bull,
	0x0000000080000001ull,
	0x8000000080008081ull,
	0x8000000000008009ull,
	0x000000000000008Aull,
	0x0000000000000088ull,
	0x0000000080008009ull,
	0x000000008000000Aull,
	0x000000008000808Bull,
	0x800000000000008Bull,
	0x8000000000008089ull,
	0x8000000000008003ull,
	0x8000000000008002ull,
	0x8000000000000080ull,
	0x000000000000800Aull,
	0x800000008000000Aull,
	0x8000000080008081ull,
	0x8000000000008080ull,
	0x0000000080000001ull,
	0x8000000080008008ull,
};

// Keccak-f[1600] of the all-zero state, as the FIPS 202 permutation gives it on the PC.
static const uint64_t zero_permuted[LANES] = {
	0xF1258F7940E1DDE7ull,
	0x84D5CCF933C0478Aull,
	0xD598261EA65AA9EEull,
	0xBD1547306F80494Dull,
	0x8B284E056253D057ull,
	0xFF97A42D7F8E6FD4ull,
	0x90FEE5A0A44647C4ull,
	0x8C5BDA0CD6192E76ull,
	0xAD30A6F71B19059Cull,
	0x30935AB7D08FFC64ull,
	0xEB5AA93F2317D635ull,
	0xA9A6E6260D712103ull,
	0x81A57C16DBCF555Full,
	0x43B831CD0347C826ull,
	0x01F22F1A11A5569Full,
	0x05E5635A21D9AE61ull,
	0x64BEFEF28CC970F2ull,
	0x613670957BC46611ull,
	0xB87C5A554FD00ECBull,
	0x8C3EE88A1CCF32C8ull,
	0x940C7922AE3A2614ull,
	0x1841F924A2C509E4ull,
	0x16F53526E70465C2ull,
	0x75F644E97F30A13Bull,
	0xEAF1FF7B5CECA249ull,
};

static inline uint64_t rotate(uint64_t value, unsigned bits)
{
	return bits ? value << bits | value >> (64 - bits) : value;
}

// Lane x + 5y is A[x, y]; theta, then rho and pi into B[y, 2x + 3y] = ROT(A[x, y], r[x, y]), chi and iota, as
// FIPS 202, section 3.2, gives them. Every lane is a variable of its own, so the round keeps more 64-bit values live
// than the GPRs hold, and GCC parks some elsewhere.
__attribute__((noinline)) static void permute(uint64_t *lanes)
{
	uint64_t a0 = lanes[0];
	uint64_t a1 = lanes[1];
	uint64_t a2 = lanes[2];
	uint64_t a3 = lanes[3];
	uint64_t a4 = lanes[4];
	uint64_t a5 = lanes[5];
	uint64_t a6 = lanes[6];
	uint64_t a7 = lanes[7];
	uint64_t a8 = lanes[8];
	uint64_t a9 = lanes[9];
	uint64_t a10 = lanes[10];
	uint64_t a11 = lanes[11];
	uint64_t a12 = lanes[12];
	uint64_t a13 = lanes[13];
	uint64_t a14 = lanes[14];
	uint64_t a15 = lanes[15];
	uint64_t a16 = lanes[16];
	uint64_t a17 = lanes[17];
	uint64_t a18 = lanes[18];
	uint64_t a19 = lanes[19];
	uint64_t a20 = lanes[20];
	uint64_t a21 = lanes[21];
	uint64_t a22 = lanes[22];
	uint64_t a23 = lanes[23];
	uint64_t a24 = lanes[24];
	for (int round = 0; round < ROUNDS; round++) {
		const uint64_t c0 = a0 ^ a5 ^ a10 ^ a15 ^ a20, c1 = a1 ^ a6 ^ a11 ^ a16 ^ a21, c2 = a2 ^ a7 ^ a12 ^ a17 ^ a22,
					   c3 = a3 ^ a8 ^ a13 ^ a18 ^ a23, c4 = a4 ^ a9 ^ a14 ^ a19 ^ a24;
		const uint64_t d0 = c4 ^ rotate(c1, 1), d1 = c0 ^ rotate(c2, 1), d2 = c1 ^ rotate(c3, 1),
					   d3 = c2 ^ rotate(c4, 1), d4 = c3 ^ rotate(c0, 1);
		a0 ^= d0;
		a1 ^= d1;
		a2 ^= d2;
		a3 ^= d3;
		a4 ^= d4;
		a5 ^= d0;
		a6 ^= d1;
		a7 ^= d2;
		a8 ^= d3;
		a9 ^= d4;
		a10 ^= d0;
		a11 ^= d1;
		a12 ^= d2;
		a13 ^= d3;
		a14 ^= d4;
		a15 ^= d0;
		a16 ^= d1;
		a17 ^= d2;
		a18 ^= d3;
		a19 ^= d4;
		a20 ^= d0;
		a21 ^= d1;
		a22 ^= d2;
		a23 ^= d3;
		a24 ^= d4;
		const uint64_t b0 = rotate(a0, 0);
		const uint64_t b10 = rotate(a1, 1);
		const uint64_t b20 = rotate(a2, 62);
		const uint64_t b5 = rotate(a3, 28);
		const uint64_t b15 = rotate(a4, 27);
		const uint64_t b16 = rotate(a5, 36);
		const uint64_t b1 = rotate(a6, 44);
		const uint64_t b11 = rotate(a7, 6);
		const uint64_t b21 = rotate(a8, 55);
		const uint64_t b6 = rotate(a9, 20);
		const uint64_t b7 = rotate(a10, 3);
		const uint64_t b17 = rotate(a11, 10);
		const uint64_t b2 = rotate(a12, 43);
		const uint64_t b12 = rotate(a13, 25);
		const uint64_t b22 = rotate(a14, 39);
		const uint64_t b23 = rotate(a15, 41);
		const uint64_t b8 = rotate(a16, 45);
		const uint64_t b18 = rotate(a17, 15);
		const uint64_t b3 = rotate(a18, 21);
		const uint64_t b13 = rotate(a19, 8);
		const uint64_t b14 = rotate(a20, 18);
		const uint64_t b24 = rotate(a21, 2);
		const uint64_t b9 = rotate(a22, 61);
		const uint64_t b19 = rotate(a23, 56);
		const uint64_t b4 = rotate(a24, 14);
		a0 = b0 ^ (~b1 & b2);
		a1 = b1 ^ (~b2 & b3);
		a2 = b2 ^ (~b3 & b4);
		a3 = b3 ^ (~b4 & b0);
		a4 = b4 ^ (~b0 & b1);
		a5 = b5 ^ (~b6 & b7);
		a6 = b6 ^ (~b7 & b8);
		a7 = b7 ^ (~b8 & b9);
		a8 = b8 ^ (~b9 & b5);
		a9 = b9 ^ (~b5 & b6);
		a10 = b10 ^ (~b11 & b12);
		a11 = b11 ^ (~b12 & b13);
		a12 = b12 ^ (~b13 & b14);
		a13 = b13 ^ (~b14 & b10);
		a14 = b14 ^ (~b10 & b11);
		a15 = b15 ^ (~b16 & b17);
		a16 = b16 ^ (~b17 & b18);
		a17 = b17 ^ (~b18 & b19);
		a18 = b18 ^ (~b19 & b15);
		a19 = b19 ^ (~b15 & b16);
		a20 = b20 ^ (~b21 & b22);
		a21 = b21 ^ (~b22 & b23);
		a22 = b22 ^ (~b23 & b24);
		a23 = b23 ^ (~b24 & b20);
		a24 = b24 ^ (~b20 & b21);
		a0 ^= round_constants[round];
	}
	lanes[0] = a0;
	lanes[1] = a1;
	lanes[2] = a2;
	lanes[3] = a3;
	lanes[4] = a4;
	lanes[5] = a5;
	lanes[6] = a6;
	lanes[7] = a7;
	lanes[8] = a8;
	lanes[9] = a9;
	lanes[10] = a10;
	lanes[11] = a11;
	lanes[12] = a12;
	lanes[13] = a13;
	lanes[14] = a14;
	lanes[15] = a15;
	lanes[16] = a16;
	lanes[17] = a17;
	lanes[18] = a18;
	lanes[19] = a19;
	lanes[20] = a20;
	lanes[21] = a21;
	lanes[22] = a22;
	lanes[23] = a23;
	lanes[24] = a24;
}

static uint32_t time_base(void)
{
	uint32_t ticks;
	__asm__ volatile("mftb %0" : "=r"(ticks));
	return ticks;
}

void test_interrupts(void)
{
	uint32_t runs = 0;
	uint32_t wrong = 0;
	const uint32_t start = time_base();
	while (time_base() - start < RUN_TICKS) {
		uint64_t lanes[LANES] = { 0 };
		permute(lanes);
		wrong += memcmp(lanes, zero_permuted, sizeof(lanes)) != 0;
		runs++;
	}
	xsdk_log_write(
		"interrupts: %u permutations in half a second of clock interrupts, %u wrong", (unsigned)runs, (unsigned)wrong);
	check("interrupts: 64-bit values held across interrupts come through whole", runs > 0 && wrong == 0);
}
