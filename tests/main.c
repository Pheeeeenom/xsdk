#include "tests.h"

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

enum { SPLASH_MS = 2000 };

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
	check("splash: the vulkan4xenon logo shown", NT_SUCCESS(xsdk_splash_show(SPLASH_MS)));
	test_image();
	test_threads();
	test_pthread();
	test_kernel_entry();
	test_files();
	test_copy();
	test_cpp_objects();
	test_cpp_threads();
	test_cpp_atomics();
	test_cpp_exceptions();
	test_cpp_chrono();
	test_cpp_filesystem();
	test_cpp_format();
	test_input();
	test_audio();
	test_gpu();
	test_present();
	test_triangle();
	test_texture();
	test_render();
	test_pacing();
	test_scale();
	test_constants();
	test_indexed();
	test_depth();
	test_raster();
	test_sampling();
	test_formats();
	test_vertex();
	test_replay();
	xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
	return failed_count;
}
