#include <chrono>
#include <format>
#include <string>
#include <thread>

#include "tests.h"
#include "xsdk/log.h"

namespace
{

constexpr std::chrono::milliseconds DELAY{ 250 };
// sleep_for is nanosleep, which runs a little long, never short.
constexpr std::chrono::milliseconds LATEST{ 400 };

} // namespace

extern "C" void test_cpp_chrono(void)
{
	using namespace std::chrono;
	const auto start = steady_clock::now();
	std::this_thread::sleep_for(DELAY);
	const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start);
	xsdk_log_write("chrono: a %lld ms sleep took %lld ms on steady_clock", static_cast<long long>(DELAY.count()),
		static_cast<long long>(elapsed.count()));
	check("steady_clock times a known delay", elapsed >= DELAY && elapsed < LATEST);

	const auto now = floor<seconds>(system_clock::now());
	const std::string date = std::format("{:%Y-%m-%d %H:%M:%S}", now);
	xsdk_log_write("chrono: system_clock reads %s UTC", date.c_str());
	const int year = static_cast<int>(year_month_day(floor<days>(now)).year());
	check("system_clock gives a plausible date", year >= 2005 && year < 2100);
}
