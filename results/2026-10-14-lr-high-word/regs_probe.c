// Local probe: do 64-bit values in r6-r12 and r14-r31 survive half a second of interrupts on the console?
#include <stdint.h>
#include <stdio.h>

enum { REGISTERS = 25, ROUNDS = 10, TICKS = 25000000 };

uint32_t regs_hold(uint32_t ticks, const uint64_t *patterns, uint64_t *out);

static const char *const names[REGISTERS] = { "r6", "r7", "r8", "r9", "r10", "r12", "r14", "r15", "r16", "r17", "r18",
	"r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31", "r11" };

int main(void)
{
	static uint64_t patterns[REGISTERS] __attribute__((aligned(8)));
	static uint64_t out[REGISTERS] __attribute__((aligned(8)));
	for (int i = 0; i < REGISTERS; i++)
		patterns[i] = 0xA5000000C3000000ull | (uint64_t)(i + 1) << 32 | (uint64_t)(i + 1);
	unsigned changed_rounds = 0;
	for (int round = 0; round < ROUNDS; round++) {
		const uint32_t loops = regs_hold(TICKS, patterns, out);
		unsigned changed = 0;
		for (int i = 0; i < REGISTERS; i++) {
			if (out[i] != patterns[i]) {
				printf("regs: round %d: %s held %016llx, given %016llx\n", round, names[i],
					(unsigned long long)out[i], (unsigned long long)patterns[i]);
				changed++;
			}
		}
		printf("regs: round %d: %u loops, %u registers changed\n", round, (unsigned)loops, changed);
		changed_rounds += changed != 0;
	}
	printf("regs probe: %u of %d rounds changed a register\n", changed_rounds, ROUNDS);
	return 0;
}
