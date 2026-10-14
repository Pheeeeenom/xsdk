// xxh_check.c: hashes fixed data with the xxhash library a build links, to compare a console build with the host's.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#ifdef __XSDK__
#include "xsdk/log.h"
#define REPORT(...) xsdk_log_write(__VA_ARGS__)
#else
#define REPORT(...) (printf(__VA_ARGS__), printf("\n"))
#endif

static uint8_t data[4200];

static uint64_t mix(uint64_t sum, uint64_t value)
{
	return (sum << 7 | sum >> 57) ^ value;
}

int main(void)
{
	for (uint32_t i = 0; i < sizeof data; i++)
		data[i] = (uint8_t)(i * 7 + 3) ^ (uint8_t)(i >> 3);
	static const uint32_t big[] = { 512, 1000, 1024, 2048, 3000, 4096 };
	uint64_t sum3 = 0, sum3seed = 0, sum64 = 0, sum32 = 0, sum128 = 0;
	uint32_t first_bad = 0xFFFFFFFF;
	for (uint32_t offset = 0; offset < 4; offset++) {
		for (uint32_t i = 0; i < 301 + 6; i++) {
			const uint32_t length = i < 301 ? i : big[i - 301];
			const uint8_t *const p = data + offset;
			const uint64_t seed = length * 0x9E3779B1u + offset;
			sum3 = mix(sum3, XXH3_64bits(p, length));
			sum3seed = mix(sum3seed, XXH3_64bits_withSeed(p, length, seed));
			sum64 = mix(sum64, XXH64(p, length, seed));
			sum32 = mix(sum32, XXH32(p, length, (uint32_t)seed));
			const XXH128_hash_t h = XXH3_128bits_withSeed(p, length, seed);
			sum128 = mix(mix(sum128, h.low64), h.high64);
			// The streaming hash, fed in pieces, must equal the one-shot one.
			XXH3_state_t state;
			XXH3_64bits_reset_withSeed(&state, seed);
			for (uint32_t done = 0, piece = 1; done < length; done += piece, piece = piece * 3 % 97 + 1)
				XXH3_64bits_update(&state, p + done, piece < length - done ? piece : length - done);
			if (XXH3_64bits_digest(&state) != XXH3_64bits_withSeed(p, length, seed) && first_bad == 0xFFFFFFFF)
				first_bad = offset << 16 | length;
		}
	}
	REPORT("xxh check: XXH3_64bits %016llx, withSeed %016llx, XXH64 %016llx, XXH32 %016llx, XXH3_128 %016llx",
		(unsigned long long)sum3, (unsigned long long)sum3seed, (unsigned long long)sum64, (unsigned long long)sum32,
		(unsigned long long)sum128);
	REPORT("xxh check: streaming %s (first differing offset<<16|length %08x)",
		first_bad == 0xFFFFFFFF ? "matches one-shot" : "DIFFERS", (unsigned)first_bad);
	return 0;
}
