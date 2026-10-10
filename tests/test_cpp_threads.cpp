#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "tests.h"

namespace
{

thread_local int thread_tag = -1;

struct Rally {
	std::mutex mutex;
	std::condition_variable turn_changed;
	int turn = 0;
	int hits = 0;
};

// Two players take turns, so every hit hands the rally over through the mutex and condition variable.
void play(Rally &rally, int player, int rounds, int &tag_seen)
{
	thread_tag = player;
	for (int i = 0; i < rounds; i++) {
		std::unique_lock lock(rally.mutex);
		rally.turn_changed.wait(lock, [&] { return rally.turn == player; });
		rally.hits++;
		rally.turn = 1 - player;
		rally.turn_changed.notify_all();
	}
	tag_seen = thread_tag;
}

} // namespace

extern "C" void test_cpp_threads(void)
{
	constexpr int rounds = 100;
	Rally rally;
	int tags[2] = { -1, -1 };
	thread_tag = 7;
	{
		const std::jthread first(play, std::ref(rally), 0, rounds, std::ref(tags[0]));
		const std::jthread second(play, std::ref(rally), 1, rounds, std::ref(tags[1]));
	}
	check("jthreads share a mutex and condition variable", rally.hits == 2 * rounds);
	check("thread_local differs per thread", tags[0] == 0 && tags[1] == 1 && thread_tag == 7);
}
