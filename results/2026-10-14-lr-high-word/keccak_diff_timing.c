// Local probe: mbedTLS's Keccak permutation compiled at -O2 against the same at -O0, on the same states.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/sha3.h"
#include "xsdk/thread.h"

enum { PERMUTATIONS = 50000, REPEATS = 10, LANES = 25, HARDWARE_THREADS = 6 };

void keccak_o2(mbedtls_sha3_context *ctx);
void keccak_o0(mbedtls_sha3_context *ctx);
void keccak_nu(mbedtls_sha3_context *ctx);
void keccak_nr(mbedtls_sha3_context *ctx);
void keccak_tu(mbedtls_sha3_context *ctx);

static void log_state(const char *what, const uint64_t *s)
{
	printf("keccak:   %s:", what);
	for (int i = 0; i < LANES; i++)
		printf(" %016llx", (unsigned long long)s[i]);
	printf("\n");
}

// Fills the stack below the caller with pattern, where the next call's frame will be.
__attribute__((noinline, unused)) static void scribble(uint32_t pattern)
{
	volatile uint32_t below[1024];
	for (int i = 0; i < 1024; i++)
		below[i] = pattern;
}

static unsigned run(void (*fast_permute)(mbedtls_sha3_context *), uint64_t seed)
{
	static mbedtls_sha3_context fast, slow;
	for (int i = 0; i < LANES; i++) {
		seed ^= seed << 13;
		seed ^= seed >> 7;
		seed ^= seed << 17;
		fast.state[i] = seed;
	}
	unsigned mismatches = 0;
	for (int n = 0; n < PERMUTATIONS; n++) {
		slow = fast;
		fast_permute(&fast);
		keccak_o0(&slow);
		if (memcmp(fast.state, slow.state, sizeof(fast.state)) != 0) {
			mismatches++;
			fast = slow;
		}
	}
	return mismatches;
}

static inline uint64_t time_base(void)
{
	uint64_t ticks;
	__asm__ volatile("mftb %0" : "=r"(ticks));
	return ticks;
}

enum { MAX_FAILURES = 4096, TB_PER_US = 50 };

int main(void)
{
	static mbedtls_sha3_context input, expected, work;
	static uint64_t when[MAX_FAILURES];
	uint64_t seed = 0x0123456789ABCDEFull;
	for (int i = 0; i < LANES; i++) {
		seed ^= seed << 13;
		seed ^= seed >> 7;
		seed ^= seed << 17;
		input.state[i] = seed;
	}
	expected = input;
	keccak_o0(&expected);

	const uint64_t start = time_base(), end = start + 2ull * 1000000 * TB_PER_US;
	uint32_t runs = 0, failures = 0;
	while (time_base() < end) {
		work = input;
		keccak_o2(&work);
		runs++;
		if (memcmp(work.state, expected.state, sizeof(work.state)) == 0)
			continue;
		if (failures < MAX_FAILURES)
			when[failures] = time_base() - start;
		if (failures < 4) {
			printf("keccak: failure %u at %llu us, lanes wrong:", (unsigned)failures,
				(unsigned long long)(when[failures] / TB_PER_US));
			for (int i = 0; i < LANES; i++)
				printf(" %d", __builtin_popcountll(work.state[i] ^ expected.state[i]));
			printf("\n");
		}
		failures++;
	}
	printf("keccak: %u runs in 2 s, %u failed\n", (unsigned)runs, (unsigned)failures);
	printf("keccak: gaps between failures in us:");
	for (uint32_t i = 1; i < failures && i < 80; i++)
		printf(" %llu", (unsigned long long)((when[i] - when[i - 1]) / TB_PER_US));
	printf("\n");
	return 0;
}
