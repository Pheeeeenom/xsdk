#include "tests.h"

#include <altivec.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "xsdk/log.h"

// The registers the console's own code keeps across a call: r14-r31 whole, f14-f31 and v14-v31 (docs/HARDWARE.md,
// "The console's calling convention, register by register"). GCC's code, called as the kernel calls a title's, must
// give them back.

enum {
	// v14-v19: kept by the console's code, and GCC's only since -mcall-xsdk.
	VR_NEWLY_KEPT = 6,
};

extern const uint64_t test_entry_gpr_sentinels[ENTRY_REGISTERS];
extern const uint64_t test_entry_fpr_sentinels[ENTRY_REGISTERS];
extern const uint32_t test_entry_vr_sentinels[ENTRY_REGISTERS][4];

static volatile uint32_t inputs[9] = { 3, 5, 7, 11, 13, 17, 19, 23, 200 };
static volatile uint64_t sink;
static volatile double sinkd;
static vector float vectors[21] __attribute__((aligned(16)));
// The registers as GCC's code held them, part way through.
static EntryRegs while_scalars __attribute__((aligned(16)));
static EntryRegs while_vectors __attribute__((aligned(16)));

// 64-bit values and doubles live across calls, which GCC keeps in r14-r31 and f14-f31.
static uint64_t keep_across_calls(void)
{
	uint64_t a = inputs[0], b = inputs[1], c = inputs[2], d = inputs[3], e = inputs[4], f = inputs[5], g = inputs[6];
	uint64_t h = inputs[7];
	double x = inputs[0], y = inputs[1], z = inputs[2], w = inputs[3];
	for (uint32_t i = 0; i < inputs[8]; i++) {
		a = a * 3 + b, b = b * 5 + c, c = c * 7 + d, d = d * 11 + e, e = e * 13 + f, f = f * 17 + g;
		g = g * 19 + h, h = h * 23 + a;
		x = x * 1.25 + y, y = y * 0.75 + z, z = z * 1.5 + w, w = w * 0.5 + x;
		test_entry_snapshot(&while_scalars);
	}
	sinkd = x + y + z + w;
	return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
}

// Twenty vectors live across calls, more than v20-v31 hold.
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
		test_entry_snapshot(&while_vectors);
	}
	return vec_add(vec_add(vec_add(vec_add(a, b), vec_add(c, d)), vec_add(vec_add(e, f), vec_add(g, h))),
		vec_add(vec_add(vec_add(i, j), vec_add(k, l)),
			vec_add(vec_add(vec_add(m, n), vec_add(o, p)), vec_add(vec_add(q, r), vec_add(s, t)))));
}

static void test_entry_target(void *arg)
{
	(void)arg;
	sink += keep_across_calls();
	vectors[20] = many_vectors();
}

EntryChanged test_entry_changed(const EntryRegs *regs)
{
	EntryChanged changed = { 0, 0, 0, 0 };
	for (uint32_t i = 0; i < ENTRY_REGISTERS; i++) {
		const uint32_t bit = 1u << (ENTRY_FIRST_KEPT + i);
		if ((uint32_t)regs->gpr[i] != (uint32_t)test_entry_gpr_sentinels[i])
			changed.gpr_low |= bit;
		if (regs->gpr[i] >> 32 != test_entry_gpr_sentinels[i] >> 32)
			changed.gpr_high |= bit;
		if (regs->fpr[i] != test_entry_fpr_sentinels[i])
			changed.fpr |= bit;
		if (memcmp(regs->vr[i], test_entry_vr_sentinels[i], sizeof regs->vr[i]) != 0)
			changed.vr |= bit;
	}
	return changed;
}

static void log_changed(const char *how, EntryChanged changed)
{
	xsdk_log_write("kernel entry: %s r14-r31 low halves %08X, high halves %08X, f14-f31 %08X, v14-v31 %08X", how,
		(unsigned)changed.gpr_low, (unsigned)changed.gpr_high, (unsigned)changed.fpr, (unsigned)changed.vr);
}

void test_kernel_entry(void)
{
	static EntryRegs after __attribute__((aligned(16)));
	for (uint32_t i = 0; i < 20; i++)
		vectors[i] = (vector float){ 1.0f, 0.5f, 0.25f, 0.125f };
	memset(&after, 0, sizeof after);
	test_entry_regs_call(test_entry_target, NULL, &after);

	const EntryChanged scalars = test_entry_changed(&while_scalars);
	const EntryChanged vectors_held = test_entry_changed(&while_vectors);
	const EntryChanged returned = test_entry_changed(&after);
	log_changed("while holding 64-bit values and doubles, GCC's code had in", scalars);
	log_changed("while holding twenty vectors, GCC's code had in", vectors_held);
	log_changed("after it returned, its caller found changed", returned);
	check("kernel entry: GCC's code held 64-bit values in r14-r31, high halves too", scalars.gpr_high != 0);
	check("kernel entry: GCC's code held doubles in f14-f31", scalars.fpr != 0);
	const uint32_t newly_kept = ((1u << VR_NEWLY_KEPT) - 1) << ENTRY_FIRST_KEPT;
	check("kernel entry: GCC's code held vectors in v14-v19", (vectors_held.vr & newly_kept) != 0);
	check("kernel entry: called as the kernel calls it, GCC's code gives back r14-r31 whole, f14-f31 and v14-v31",
		!returned.gpr_low && !returned.gpr_high && !returned.fpr && !returned.vr);
}
