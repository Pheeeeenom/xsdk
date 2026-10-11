#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"

// The SDK's memcpy and memmove (src/copy.c), checked byte for byte, with the bytes around each copy left alone.

enum {
	MAX_SMALL_BYTES = 300,
	ALIGNMENTS = 16,
	GUARD_BYTES = 160,
	BUFFER_BYTES = GUARD_BYTES + ALIGNMENTS + MAX_SMALL_BYTES + GUARD_BYTES,
	LARGE_BYTES = 1024 * 1024 + 77,
	SENTINEL = 0xA5,
};

// Distances memmove moves a range by: either side of a quad and a line, and near none.
static const int32_t shifts[] = { -140, -129, -128, -127, -64, -17, -16, -15, -9, -4, -1, 1, 4, 9, 15, 16, 17, 64, 127,
	128, 129, 140 };

static uint8_t source[BUFFER_BYTES];
static uint8_t destination[BUFFER_BYTES];
static uint8_t moved[BUFFER_BYTES];

static uint8_t pattern(uint32_t i)
{
	return (uint8_t)(i * 7 + (i >> 8) + 1);
}

// Size bytes from the pattern's byte from, at buffer's byte to, and the sentinel for a guard's width either side.
static bool copied(volatile const uint8_t *buffer, uint32_t to, uint32_t size, uint32_t from)
{
	for (uint32_t i = to - GUARD_BYTES; i < to + size + GUARD_BYTES; i++) {
		const uint8_t want = i >= to && i < to + size ? pattern(from + i - to) : SENTINEL;
		if (buffer[i] != want)
			return false;
	}
	return true;
}

static bool copy_small_sizes(volatile uint8_t *out, uint32_t source_alignments, const char *where)
{
	for (uint32_t i = 0; i < BUFFER_BYTES; i++)
		source[i] = pattern(i);
	for (uint32_t size = 0; size <= MAX_SMALL_BYTES; size++) {
		for (uint32_t from = GUARD_BYTES; from < GUARD_BYTES + source_alignments; from++) {
			for (uint32_t to = GUARD_BYTES; to < GUARD_BYTES + ALIGNMENTS; to++) {
				memset((void *)out, SENTINEL, BUFFER_BYTES);
				memcpy((void *)(out + to), source + from, size);
				if (!copied(out, to, size, from)) {
					xsdk_log_write("copies: %s: %u bytes from offset %u to offset %u came out wrong", where,
						(unsigned)size, (unsigned)(from % ALIGNMENTS), (unsigned)(to % ALIGNMENTS));
					return false;
				}
			}
		}
	}
	return true;
}

static bool copy_large(void)
{
	static const uint32_t offsets[][2] = { { 0, 0 }, { 3, 0 }, { 0, 5 }, { 9, 13 } };
	uint8_t *const from = malloc(LARGE_BYTES + ALIGNMENTS);
	uint8_t *const to = malloc(LARGE_BYTES + ALIGNMENTS);
	bool right = from && to;
	for (uint32_t i = 0; right && i < LARGE_BYTES + ALIGNMENTS; i++)
		from[i] = pattern(i);
	for (uint32_t o = 0; right && o < sizeof offsets / sizeof offsets[0]; o++) {
		memcpy(to + offsets[o][1], from + offsets[o][0], LARGE_BYTES);
		for (uint32_t i = 0; right && i < LARGE_BYTES; i++)
			right = to[offsets[o][1] + i] == pattern(offsets[o][0] + i);
		if (!right)
			xsdk_log_write("copies: 1 MB from offset %u to offset %u came out wrong", (unsigned)offsets[o][0],
				(unsigned)offsets[o][1]);
	}
	free(to);
	free(from);
	return right;
}

static bool move_overlapping(void)
{
	static const uint32_t bases[] = { GUARD_BYTES, GUARD_BYTES + 7 };
	for (uint32_t b = 0; b < sizeof bases / sizeof bases[0]; b++) {
		for (uint32_t size = 0; size <= MAX_SMALL_BYTES; size++) {
			for (uint32_t s = 0; s < sizeof shifts / sizeof shifts[0]; s++) {
				const uint32_t to = (uint32_t)((int32_t)bases[b] + shifts[s]);
				for (uint32_t i = 0; i < BUFFER_BYTES; i++)
					moved[i] = pattern(i);
				memmove(moved + to, moved + bases[b], size);
				for (uint32_t i = 0; i < BUFFER_BYTES; i++) {
					const uint8_t want = i >= to && i < to + size ? pattern(bases[b] + i - to) : pattern(i);
					if (moved[i] != want) {
						xsdk_log_write(
							"copies: memmove of %u bytes by %d came out wrong", (unsigned)size, (int)shifts[s]);
						return false;
					}
				}
			}
		}
	}
	return true;
}

void test_copy(void)
{
	check("copies: memcpy, 0 to 300 bytes at every source and destination alignment",
		copy_small_sizes(destination, ALIGNMENTS, "cached memory"));
	check("copies: memcpy, 1 MB at four alignments", copy_large());
	volatile uint8_t *const combined = (volatile uint8_t *)xsdk_gpu_alloc(BUFFER_BYTES);
	check("copies: memcpy into write-combined memory, 0 to 300 bytes at every destination alignment, from two sources",
		combined && copy_small_sizes(combined, 2, "write-combined memory"));
	xsdk_gpu_free((volatile uint32_t *)combined);
	check("copies: memmove, ranges moved up and down over themselves", move_overlapping());
}
