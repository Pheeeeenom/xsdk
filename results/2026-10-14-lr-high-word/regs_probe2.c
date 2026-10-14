// Local probe: do r0, r3-r5, r10-r12, CTR, CR and XER survive half a second of interrupts on the console?
#include <stdint.h>
#include <stdio.h>

enum { HELD = 10, ROUNDS = 10, TICKS = 25000000 };

uint32_t regs_hold2(uint32_t ticks, const uint64_t *patterns, uint64_t *out);

static const char *const names[HELD] = { "r0", "r3", "r4", "r5", "r10", "r11", "r12", "ctr", "cr", "xer" };

int main(void)
{
	static uint64_t patterns[HELD] __attribute__((aligned(8)));
	static uint64_t out[HELD] __attribute__((aligned(8)));
	for (int i = 0; i < 7; i++)
		patterns[i] = 0xB6000000D7000000ull | (uint64_t)(i + 1) << 32 | (uint64_t)(i + 1);
	patterns[7] = 0x9100000012345678ull;
	patterns[8] = 0x0000000002468ACEull;
	patterns[9] = 0x0000000020000045ull;
	unsigned changed_rounds = 0;
	for (int round = 0; round < ROUNDS; round++) {
		(void)regs_hold2(TICKS, patterns, out);
		unsigned changed = 0;
		for (int i = 0; i < HELD; i++) {
			uint64_t held = out[i], given = patterns[i];
			if (i == 8) {
				// CR0 is the loop's; CR1-CR7 are the pattern's.
				held &= 0x0FFFFFFF;
				given &= 0x0FFFFFFF;
			}
			if (i == 9) {
				// XER: SO, OV, CA and the byte count.
				held &= 0xE000007F;
				given &= 0xE000007F;
			}
			if (held != given) {
				printf("regs2: round %d: %s held %016llx, given %016llx\n", round, names[i], (unsigned long long)out[i],
					(unsigned long long)patterns[i]);
				changed++;
			}
		}
		changed_rounds += changed != 0;
	}
	printf("regs2 probe: %u of %d rounds changed something\n", changed_rounds, ROUNDS);
	return 0;
}
