#include <stdint.h>
#include <string.h>

#include "xsdk/log.h"

// volatile keeps the compiler from answering these checks at build time.
static const volatile char rodata_text[] = "rodata";
static volatile uint32_t data_word = 0x12345678;
static volatile uint32_t bss_words[1024];

static void check(const char *name, int passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
}

static int bss_is_zero(void)
{
	for (size_t i = 0; i < sizeof(bss_words) / sizeof(bss_words[0]); i++) {
		if (bss_words[i])
			return 0;
	}
	return 1;
}

int main(void)
{
	xsdk_log_write("xsdk test title");
	check("read-only data", strcmp((const char *)rodata_text, "rodata") == 0);
	check("initialised data", data_word == 0x12345678);
	check("zeroed data", bss_is_zero());
	data_word = 0x9ABCDEF0;
	check("data is writable", data_word == 0x9ABCDEF0);
	return 0;
}
