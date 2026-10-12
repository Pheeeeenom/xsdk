#include "tests.h"

#include <altivec.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "xsdk/log.h"

// The registers the kernel's code keeps across a call, as it calls a title's code (docs/HARDWARE.md, "Calls between
// xsdk's GCC and the console's MSVC").

enum {
	REGISTERS = 18,
	FIRST_KEPT = 14,
	// v14-v19: kept by the kernel's code, used freely by GCC's unless the toolchain reserves them.
	VR_GCC_FREE = 6,
};

typedef struct {
	uint64_t gpr[REGISTERS];
	uint64_t fpr[REGISTERS];
	uint32_t vr[REGISTERS][4];
} EntryRegs;

void test_entry_regs_call(void (*fn)(void *), void *arg, EntryRegs *out);
void test_entry_wrapped(void *arg);
void test_entry_target(void *arg);
extern const uint64_t test_entry_gpr_sentinels[REGISTERS];
extern const uint64_t test_entry_fpr_sentinels[REGISTERS];
extern const uint32_t test_entry_vr_sentinels[REGISTERS][4];

typedef struct {
	uint32_t gpr_low;
	uint32_t gpr_high;
	uint32_t fpr;
	uint32_t vr;
} Changed;

static volatile uint32_t inputs[9] = { 3, 5, 7, 11, 13, 17, 19, 23, 200 };
static volatile uint64_t sink;
static vector float vectors[21] __attribute__((aligned(16)));

__attribute__((noinline)) static void step(void)
{
	sink = sink + 1;
}

// 64-bit values live across calls, which GCC keeps in r14-r31 and restores only by their low halves.
static uint64_t keep_across_calls(void)
{
	uint64_t a = inputs[0], b = inputs[1], c = inputs[2], d = inputs[3], e = inputs[4], f = inputs[5], g = inputs[6];
	uint64_t h = inputs[7];
	for (uint32_t i = 0; i < inputs[8]; i++) {
		a = a * 3 + b, b = b * 5 + c, c = c * 7 + d, d = d * 11 + e, e = e * 13 + f, f = f * 17 + g;
		g = g * 19 + h, h = h * 23 + a;
		step();
	}
	return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
}

// Twenty vectors live at once, which takes v14-v19 unless the toolchain reserves them.
static vector float many_vectors(void)
{
	vector float a = vectors[0], b = vectors[1], c = vectors[2], d = vectors[3], e = vectors[4], f = vectors[5];
	vector float g = vectors[6], h = vectors[7], i = vectors[8], j = vectors[9], k = vectors[10], l = vectors[11];
	vector float m = vectors[12], n = vectors[13], o = vectors[14], p = vectors[15], q = vectors[16];
	vector float r = vectors[17], s = vectors[18], t = vectors[19];
	for (uint32_t z = 0; z < inputs[8]; z++) {
		a = vec_madd(a, b, c), b = vec_madd(b, c, d), c = vec_madd(c, d, e), d = vec_madd(d, e, f);
		e = vec_madd(e, f, g), f = vec_madd(f, g, h), g = vec_madd(g, h, i), h = vec_madd(h, i, j);
		i = vec_madd(i, j, k), j = vec_madd(j, k, l), k = vec_madd(k, l, m), l = vec_madd(l, m, n);
		m = vec_madd(m, n, o), n = vec_madd(n, o, p), o = vec_madd(o, p, q), p = vec_madd(p, q, r);
		q = vec_madd(q, r, s), r = vec_madd(r, s, t), s = vec_madd(s, t, a), t = vec_madd(t, a, b);
	}
	return vec_add(vec_add(vec_add(vec_add(a, b), vec_add(c, d)), vec_add(vec_add(e, f), vec_add(g, h))),
		vec_add(vec_add(vec_add(i, j), vec_add(k, l)),
			vec_add(vec_add(vec_add(m, n), vec_add(o, p)), vec_add(vec_add(q, r), vec_add(s, t)))));
}

void test_entry_target(void *arg)
{
	(void)arg;
	sink += keep_across_calls();
	vectors[20] = many_vectors();
}

static Changed call_and_compare(void (*fn)(void *))
{
	static EntryRegs regs __attribute__((aligned(16)));
	memset(&regs, 0, sizeof regs);
	test_entry_regs_call(fn, NULL, &regs);
	Changed changed = { 0, 0, 0, 0 };
	for (uint32_t i = 0; i < REGISTERS; i++) {
		const uint32_t bit = 1u << (FIRST_KEPT + i);
		if ((uint32_t)regs.gpr[i] != (uint32_t)test_entry_gpr_sentinels[i])
			changed.gpr_low |= bit;
		if (regs.gpr[i] >> 32 != test_entry_gpr_sentinels[i] >> 32)
			changed.gpr_high |= bit;
		if (regs.fpr[i] != test_entry_fpr_sentinels[i])
			changed.fpr |= bit;
		if (memcmp(regs.vr[i], test_entry_vr_sentinels[i], sizeof regs.vr[i]) != 0)
			changed.vr |= bit;
	}
	return changed;
}

static void log_changed(const char *how, Changed changed)
{
	xsdk_log_write("kernel entry: %s changed r14-r31 low halves %08X, high halves %08X, f14-f31 %08X, v14-v31 %08X",
		how, (unsigned)changed.gpr_low, (unsigned)changed.gpr_high, (unsigned)changed.fpr, (unsigned)changed.vr);
}

void test_kernel_entry(void)
{
	for (uint32_t i = 0; i < 20; i++)
		vectors[i] = (vector float){ 1.0f, 0.5f, 0.25f, 0.125f };
	const Changed direct = call_and_compare(test_entry_target);
	const Changed wrapped = call_and_compare(test_entry_wrapped);
	log_changed("GCC code called directly", direct);
	log_changed("the same code through XSDK_KERNEL_ENTRY", wrapped);
	check("kernel entry: the GCC code under test does change r14-r31's high halves when called directly",
		direct.gpr_high != 0);
	check("kernel entry: through XSDK_KERNEL_ENTRY, r14-r31 come back whole and f14-f31 and v14-v31 unchanged",
		!wrapped.gpr_low && !wrapped.gpr_high && !wrapped.fpr && !wrapped.vr);
	const uint32_t gcc_free_vrs = ((1u << VR_GCC_FREE) - 1) << FIRST_KEPT;
	check("kernel entry: GCC code using twenty vectors leaves v14-v19 alone", !(direct.vr & gcc_free_vrs));
}
