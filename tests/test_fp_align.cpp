#include <bit>
#include <cstdint>
#include <cstring>
#include <utility>

#include "tests.h"
#include "xsdk/log.h"

// Floats and doubles at every byte offset. The console faults on a float register load or store at an address not
// aligned to its size, while integer loads and stores take any address, so the compiler must move these through the
// integer registers (SCOPE.md, milestone 23). Xenia doesn't check alignment: only the console tells.

namespace
{

constexpr unsigned OFFSETS = 8;

// Each access in a function nothing can see into, so each one goes to memory.

[[gnu::noipa]] void write_float_memcpy(unsigned char *p, float value)
{
	std::memcpy(p, &value, sizeof value);
}

[[gnu::noipa]] float read_float_memcpy(const unsigned char *p)
{
	float value;
	std::memcpy(&value, p, sizeof value);
	return value;
}

[[gnu::noipa]] void write_float_bits(unsigned char *p, float value)
{
	const uint32_t bits = std::bit_cast<uint32_t>(value);
	std::memcpy(p, &bits, sizeof bits);
}

// As Aurora's read_bits<float> does: an integer from the bytes, then the float with its bits.
[[gnu::noipa]] float read_float_bits(const unsigned char *p)
{
	uint32_t bits;
	std::memcpy(&bits, p, sizeof bits);
	return std::bit_cast<float>(bits);
}

[[gnu::noipa]] void write_double_memcpy(unsigned char *p, double value)
{
	std::memcpy(p, &value, sizeof value);
}

[[gnu::noipa]] double read_double_memcpy(const unsigned char *p)
{
	double value;
	std::memcpy(&value, p, sizeof value);
	return value;
}

[[gnu::noipa]] void write_double_bits(unsigned char *p, double value)
{
	const uint64_t bits = std::bit_cast<uint64_t>(value);
	std::memcpy(p, &bits, sizeof bits);
}

[[gnu::noipa]] double read_double_bits(const unsigned char *p)
{
	uint64_t bits;
	std::memcpy(&bits, p, sizeof bits);
	return std::bit_cast<double>(bits);
}

// A float at byte N and a double at N + 4.
template <unsigned N> struct [[gnu::packed]] Packed {
	unsigned char pad[N];
	float f;
	double d;
};

template <unsigned N> [[gnu::noipa]] void write_packed(unsigned char *p, float f, double d)
{
	Packed<N> *packed = reinterpret_cast<Packed<N> *>(p);
	packed->f = f;
	packed->d = d;
}

template <unsigned N> [[gnu::noipa]] float read_packed_float(const unsigned char *p)
{
	return reinterpret_cast<const Packed<N> *>(p)->f;
}

template <unsigned N> [[gnu::noipa]] double read_packed_double(const unsigned char *p)
{
	return reinterpret_cast<const Packed<N> *>(p)->d;
}

bool same(float a, float b)
{
	return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
}

bool same(double a, double b)
{
	return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
}

struct Tally {
	unsigned reads = 0;
	unsigned wrong = 0;

	void read(bool right)
	{
		reads++;
		wrong += !right;
	}
};

alignas(16) unsigned char buffer[32];

template <unsigned N> void packed_at(Tally &tally)
{
	const float f = 0.75f + N;
	const double d = 1024.125 + N;
	write_packed<N>(buffer, f, d);
	tally.read(same(read_packed_float<N>(buffer), f));
	tally.read(same(read_packed_double<N>(buffer), d));
	tally.read(same(read_float_bits(buffer + N), f));
	tally.read(same(read_double_bits(buffer + N + 4), d));
}

template <unsigned... N> void packed_all(Tally &tally, std::integer_sequence<unsigned, N...>)
{
	(packed_at<N + 1>(tally), ...);
}

} // namespace

extern "C" void test_fp_align(void)
{
	xsdk_log_write("fp align: writing and reading floats and doubles at offsets 0 to 7");
	Tally tally;
	for (unsigned offset = 0; offset < OFFSETS; offset++) {
		unsigned char *p = buffer + offset;
		const float f = 1.5f + static_cast<float>(offset);
		const double d = -2.25 - 0.5 * offset;
		write_float_memcpy(p, f);
		tally.read(same(read_float_memcpy(p), f));
		tally.read(same(read_float_bits(p), f));
		write_float_bits(p, -f);
		tally.read(same(read_float_memcpy(p), -f));
		write_double_memcpy(p, d);
		tally.read(same(read_double_memcpy(p), d));
		tally.read(same(read_double_bits(p), d));
		write_double_bits(p, -d);
		tally.read(same(read_double_memcpy(p), -d));
	}
	packed_all(tally, std::make_integer_sequence<unsigned, OFFSETS - 1>{});
	xsdk_log_write("fp align: %u of %u reads gave back other bits", tally.wrong, tally.reads);
	check(
		"fp align: floats and doubles written and read back at every offset from 0 to 7, through memcpy, "
		"std::bit_cast and packed structs",
		tally.reads && !tally.wrong);
}
