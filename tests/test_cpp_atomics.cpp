#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "tests.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

namespace
{

constexpr int WORKERS = 4;
constexpr int INCREMENTS = 10000;
// Starts just below 2^32, so the count has to carry into the upper word.
constexpr std::uint64_t WIDE_START = 0xFFFFFFF0ull;
// libstdc++ spins briefly before it blocks, so a sleeping waiter yields a few times; a spinning one, thousands.
constexpr std::uint32_t MAX_SLEEPING_YIELDS = 100;

void count_concurrently()
{
	std::atomic<std::uint32_t> narrow{ 0 };
	std::atomic<std::uint64_t> wide{ WIDE_START };
	{
		std::vector<std::jthread> workers;
		for (int t = 0; t < WORKERS; t++) {
			workers.emplace_back([&] {
				for (int i = 0; i < INCREMENTS; i++) {
					narrow.fetch_add(1, std::memory_order_relaxed);
					wide.fetch_add(1, std::memory_order_relaxed);
				}
			});
		}
	}
	check("32-bit atomic count", narrow == WORKERS * INCREMENTS);
	check("64-bit atomic count", wide == WIDE_START + WORKERS * INCREMENTS);
}

void wait_and_notify()
{
	std::atomic<bool> ready{ false };
	std::atomic<bool> woke{ false };
	std::uint32_t yields_before;
	std::uint32_t waits_before;
	xsdk_thread_wait_counts(&yields_before, &waits_before);
	{
		const std::jthread waiter([&] {
			ready.wait(false);
			woke = true;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		ready = true;
		ready.notify_one();
	}
	std::uint32_t yields_after;
	std::uint32_t waits_after;
	xsdk_thread_wait_counts(&yields_after, &waits_after);
	const std::uint32_t yields = yields_after - yields_before;
	const std::uint32_t waits = waits_after - waits_before;
	xsdk_log_write("atomics: over a 200 ms wait the waiter yielded %u times and blocked %u times",
		static_cast<unsigned>(yields), static_cast<unsigned>(waits));
	check("atomic wait sleeps until notified", woke && waits > 0 && yields < MAX_SLEEPING_YIELDS);
}

} // namespace

extern "C" void test_cpp_atomics(void)
{
	count_concurrently();
	wait_and_notify();
}
