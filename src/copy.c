#include <altivec.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// memcpy and memmove for titles, linked ahead of newlib's, whose stores go out of order and which copies unaligned
// data a byte at a time. Long copies go through VMX, as fast as the console copied (docs/HARDWARE.md); stores go out
// in order, which write-combined memory needs, and nothing uses dcbz, so any memory works.

enum {
	QUAD_BYTES = 16,
	QUADS_PER_LINE = 8,
	// The CPU's cache line (libxenon:drivers/ppc/cache.S:1).
	LINE_BYTES = QUAD_BYTES * QUADS_PER_LINE,
	// Shorter copies go through general registers.
	VMX_MIN_BYTES = 256,
	// The source is asked for this far ahead, as the probe that measured VMX copies did.
	PREFETCH_BYTES = 8 * LINE_BYTES,
};

typedef vector unsigned char Quad;
// A word that may alias anything, as a copy's bytes do.
typedef uint32_t __attribute__((may_alias)) Word;

// Keeps GCC from moving one store past another.
static inline void barrier(void)
{
	__asm__ volatile("" ::: "memory");
}

static inline void prefetch(const uint8_t *address)
{
	__asm__ volatile("dcbt 0, %0" : : "r"(address));
}

// Words when both are 4-byte aligned, else bytes. Volatile, so GCC neither reorders the stores nor merges two into a
// 64-bit one, which faults in write-combined memory unless 8-byte aligned (docs/HARDWARE.md).
static void copy_small(uint8_t *out, const uint8_t *in, size_t size)
{
	if ((((uintptr_t)out | (uintptr_t)in) & 3) == 0) {
		for (; size >= 4; size -= 4, out += 4, in += 4)
			*(volatile Word *)out = *(const Word *)in;
	}
	for (; size; size--)
		*(volatile uint8_t *)out++ = *in++;
}

// A line through eight registers, the loads first so they overlap.
static void copy_line(uint8_t *out, const uint8_t *in)
{
	const Quad q0 = vec_ld(0, in), q1 = vec_ld(16, in), q2 = vec_ld(32, in), q3 = vec_ld(48, in);
	const Quad q4 = vec_ld(64, in), q5 = vec_ld(80, in), q6 = vec_ld(96, in), q7 = vec_ld(112, in);
	barrier();
	vec_st(q0, 0, out);
	barrier();
	vec_st(q1, 16, out);
	barrier();
	vec_st(q2, 32, out);
	barrier();
	vec_st(q3, 48, out);
	barrier();
	vec_st(q4, 64, out);
	barrier();
	vec_st(q5, 80, out);
	barrier();
	vec_st(q6, 96, out);
	barrier();
	vec_st(q7, 112, out);
	barrier();
}

// The same from a source off a 16-byte boundary: each quad is put together from the two aligned quads it spans.
// The last aligned load still holds a byte of the source, so it never reaches past the source's last block.
static Quad copy_line_shifted(uint8_t *out, const uint8_t *in, Quad previous, Quad shift)
{
	const Quad q1 = vec_ld(16, in), q2 = vec_ld(32, in), q3 = vec_ld(48, in), q4 = vec_ld(64, in);
	const Quad q5 = vec_ld(80, in), q6 = vec_ld(96, in), q7 = vec_ld(112, in), q8 = vec_ld(128, in);
	barrier();
	vec_st(vec_perm(previous, q1, shift), 0, out);
	barrier();
	vec_st(vec_perm(q1, q2, shift), 16, out);
	barrier();
	vec_st(vec_perm(q2, q3, shift), 32, out);
	barrier();
	vec_st(vec_perm(q3, q4, shift), 48, out);
	barrier();
	vec_st(vec_perm(q4, q5, shift), 64, out);
	barrier();
	vec_st(vec_perm(q5, q6, shift), 80, out);
	barrier();
	vec_st(vec_perm(q6, q7, shift), 96, out);
	barrier();
	vec_st(vec_perm(q7, q8, shift), 112, out);
	barrier();
	return q8;
}

// Whole lines to a 16-byte aligned destination; returns how many bytes it copied.
static size_t copy_lines(uint8_t *out, const uint8_t *in, size_t size)
{
	const size_t lines_bytes = size / LINE_BYTES * LINE_BYTES;
	const bool aligned = ((uintptr_t)in & (QUAD_BYTES - 1)) == 0;
	const Quad shift = vec_lvsl(0, in);
	Quad previous = vec_ld(0, in);
	for (size_t offset = 0; offset < lines_bytes; offset += LINE_BYTES) {
		if (offset + PREFETCH_BYTES < size)
			prefetch(in + offset + PREFETCH_BYTES);
		if (aligned)
			copy_line(out + offset, in + offset);
		else
			previous = copy_line_shifted(out + offset, in + offset, previous, shift);
	}
	return lines_bytes;
}

static void copy_forward(uint8_t *out, const uint8_t *in, size_t size)
{
	if (size >= VMX_MIN_BYTES) {
		const size_t lead = -(uintptr_t)out & (QUAD_BYTES - 1);
		copy_small(out, in, lead);
		const size_t copied = copy_lines(out + lead, in + lead, size - lead);
		out += lead + copied;
		in += lead + copied;
		size -= lead + copied;
	}
	copy_small(out, in, size);
}

// From the end down, for a destination that overlaps the source from above.
static void copy_backward(uint8_t *out, const uint8_t *in, size_t size)
{
	if ((((uintptr_t)out | (uintptr_t)in | size) & 3) == 0) {
		for (; size; size -= 4)
			*(volatile Word *)(out + size - 4) = *(const Word *)(in + size - 4);
		return;
	}
	for (; size; size--)
		*(volatile uint8_t *)(out + size - 1) = in[size - 1];
}

void *memcpy(void *restrict destination, const void *restrict source, size_t size)
{
	copy_forward(destination, source, size);
	return destination;
}

// Copying forward is safe while the destination starts at or below the source: each block is read before it's
// written over.
void *memmove(void *destination, const void *source, size_t size)
{
	uint8_t *const out = destination;
	const uint8_t *const in = source;
	if (out <= in || out >= in + size)
		copy_forward(out, in, size);
	else
		copy_backward(out, in, size);
	return destination;
}
