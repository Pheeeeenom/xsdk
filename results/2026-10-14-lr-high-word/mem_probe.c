// Local probe: does anything write into a title function's own stack frame while it runs on the console?
#include <stdint.h>
#include <stdio.h>

enum { ROUNDS = 20, TICKS = 12500000, MAX_CHANGED = 64 };

uint32_t mem_hold(uint32_t ticks, uint32_t *changed);

int main(void)
{
	static uint32_t changed[MAX_CHANGED * 2];
	unsigned rounds_changed = 0;
	for (int round = 0; round < ROUNDS; round++) {
		const uint32_t count = mem_hold(TICKS, changed);
		for (uint32_t i = 0; i < count; i++)
			printf("mem: round %d: %u(r1) now holds low word %08x\n", round, (unsigned)changed[2 * i],
				(unsigned)changed[2 * i + 1]);
		rounds_changed += count != 0;
	}
	printf("mem probe: %u of %d rounds found the frame changed\n", rounds_changed, ROUNDS);
	return 0;
}
