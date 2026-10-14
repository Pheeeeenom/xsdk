// Local probe: SHA3-224 of 2000 and 1000000 'a's in 1000-byte updates, against hashlib's digests.
#include <stdio.h>
#include <string.h>

#include "mbedtls/sha3.h"

#ifndef VARIANT
#define VARIANT "?"
#endif

static unsigned char buffer[1000] __attribute__((aligned(16)));

static void hash(size_t total, const char *expected)
{
	mbedtls_sha3_context ctx;
	mbedtls_sha3_init(&ctx);
	mbedtls_sha3_starts(&ctx, MBEDTLS_SHA3_224);
	for (size_t done = 0; done < total; done += 1000)
		mbedtls_sha3_update(&ctx, buffer, 1000);
	unsigned char digest[28];
	mbedtls_sha3_finish(&ctx, digest, sizeof(digest));
	char text[57];
	for (int i = 0; i < 28; i++)
		snprintf(text + 2 * i, 3, "%02x", digest[i]);
	printf("sha3 %s: %u bytes: %s %s\n", VARIANT, (unsigned)total, text, strcmp(text, expected) ? "MISMATCH" : "match");
}

int main(void)
{
	memset(buffer, 'a', sizeof(buffer));
	hash(2000, "5335b97fc4a134accb58cd795624b4eaeb1767d351cb52ebbdfb3755");
	hash(1000000, "d69335b93325192e516a912e6d19a15cb51c6ed5c15243e7a7fd653c");
	return 0;
}
