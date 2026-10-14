// Local probe: does a 64-bit value in LR, and in CTR, survive half a second of interrupts on the console?
#include <stdint.h>
#include <stdio.h>

enum { ROUNDS = 10, TICKS = 25000000 };

void lr_hold(uint32_t ticks, const uint64_t *pattern, uint64_t *out);

int main(void)
{
	static const uint64_t pattern = 0xA1B2C3D4E5F60718ull;
	static uint64_t out[2];
	unsigned lr_changed = 0, ctr_changed = 0;
	for (int round = 0; round < ROUNDS; round++) {
		lr_hold(TICKS, &pattern, out);
		printf("lr: round %d: LR %016llx, CTR %016llx\n", round, (unsigned long long)out[0], (unsigned long long)out[1]);
		lr_changed += out[0] != pattern;
		ctr_changed += out[1] != pattern;
	}
	printf("lr probe: LR changed in %u of %d rounds, CTR in %u\n", lr_changed, ROUNDS, ctr_changed);
	return 0;
}
