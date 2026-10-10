#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "tests.h"

namespace
{

int set_by_constructor;

struct Startup {
	Startup()
	{
		set_by_constructor = 42;
	}
};

Startup startup;

struct alignas(64) CacheLine {
	char bytes[64];
};

// VMX128 loads and stores need 16-byte alignment.
struct alignas(16) Vector4 {
	float lanes[4];
};

bool is_aligned(const void *pointer, std::size_t alignment)
{
	return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

} // namespace

extern "C" void test_cpp_objects(void)
{
	check("static constructors run before main", set_by_constructor == 42);

	constexpr std::size_t page_bytes = 4096;
	const auto line = std::make_unique<CacheLine>();
	const std::vector<Vector4> vectors(9);
	void *page = ::operator new(page_bytes, std::align_val_t{ page_bytes });
	check("aligned and over-aligned allocation",
		is_aligned(line.get(), 64) && is_aligned(vectors.data(), 16) && is_aligned(page, page_bytes));
	::operator delete(page, std::align_val_t{ page_bytes });
}
