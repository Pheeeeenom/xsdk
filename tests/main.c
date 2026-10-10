#include "tests.h"

#include "xsdk/log.h"

static int passed_count;
static int failed_count;

void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	if (passed)
		passed_count++;
	else
		failed_count++;
}

int main(void)
{
	xsdk_log_write("xsdk test title");
	test_image();
	test_threads();
	test_files();
	test_cpp_objects();
	test_cpp_threads();
	test_cpp_atomics();
	test_cpp_chrono();
	test_cpp_filesystem();
	test_cpp_format();
	test_input();
	test_audio();
	// Last: unwinding hangs in Xenia and is unverified on the console, so a hang here costs no other result.
	test_cpp_exceptions();
	xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
	return failed_count;
}
