#include <stdexcept>
#include <thread>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

namespace
{

enum { SPLASH_MS = 2000 };

int passed_count;
int failed_count;
bool constructed;

struct Constructed {
	Constructed()
	{
		constructed = true;
	}
};

const Constructed constructed_at_start;

void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	if (passed)
		passed_count++;
	else
		failed_count++;
}

bool caught()
{
	try {
		throw std::runtime_error("thrown");
	} catch (const std::runtime_error &) {
		return true;
	}
	return false;
}

bool joined()
{
	bool ran = false;
	std::thread thread([&ran] { ran = true; });
	thread.join();
	return ran;
}

} // namespace

int main()
{
	xsdk_log_write("cmake check title");
	check("splash: the vulkan4xenon logo shown", NT_SUCCESS(xsdk_splash_show(SPLASH_MS)));
	check("cmake: a static constructor ran before main", constructed);
	check("cmake: an exception was thrown and caught", caught());
	check("cmake: a std::thread ran and joined", joined());
	xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
	return failed_count;
}
