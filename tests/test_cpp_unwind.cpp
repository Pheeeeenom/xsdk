#include <setjmp.h>
#include <stdint.h>
#include <unwind.h>

#include "tests.h"
#include "xsdk/log.h"

// The registers a caller keeps, r14-r31 whole, f14-f31 and v14-v31, through a C++ exception and through longjmp.
// kernel_entry_check.S puts sentinels in them and calls through frames that overwrite every one, so the sentinels come
// back only if the unwinder, or setjmp and longjmp, restore them whole (docs/HARDWARE.md, "The console's calling
// convention, register by register").

extern "C" jmp_buf test_jump;
extern "C" int test_run_setjmp(void (*fn)(void));

jmp_buf test_jump;

namespace
{

constexpr int DEPTH = 3;

// The registers as the deepest frame left them, before the throw or the longjmp, and as the catch found them.
EntryRegs at_bottom __attribute__((aligned(16)));
EntryRegs at_catch __attribute__((aligned(16)));

// What the unwinder makes of each frame above the deepest, before the throw: where it returns to, its CFA, and the
// r14, r31 and f14 it held.
constexpr unsigned TRACE_FRAMES = 8;
// DWARF's column for f14.
constexpr int F14_COLUMN = 46;

struct TracedFrame {
	uintptr_t ip;
	uintptr_t cfa;
	uint64_t r14;
	uint64_t r31;
	uint64_t f14;
};

struct Trace {
	TracedFrame frames[TRACE_FRAMES];
	unsigned count;
	bool wanted;
};

Trace trace;

_Unwind_Reason_Code trace_frame(_Unwind_Context *context, void *arg)
{
	Trace *t = static_cast<Trace *>(arg);
	if (t->count == TRACE_FRAMES)
		return _URC_END_OF_STACK;
	TracedFrame &f = t->frames[t->count++];
	f.ip = _Unwind_GetIP(context);
	f.cfa = _Unwind_GetCFA(context);
	f.r14 = _Unwind_GetGR(context, 14);
	f.r31 = _Unwind_GetGR(context, 31);
	f.f14 = _Unwind_GetGR(context, F14_COLUMN);
	return _URC_NO_REASON;
}

// Overwrites r14-r31 whole, f14-f31 and v14-v31, so GCC saves them in its prologue, says where for the unwinder, and
// restores them in its epilogue.
__attribute__((noinline)) void clobber(int depth, void (*bottom)())
{
	asm volatile(
		".irp k,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n\t"
		"li \\k,-1\n\t"
		"fsub \\k,\\k,\\k\n\t"
		"vspltisw \\k,-1\n\t"
		".endr" ::
			: "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28",
		"r29", "r30", "r31", "fr14", "fr15", "fr16", "fr17", "fr18", "fr19", "fr20", "fr21", "fr22", "fr23", "fr24",
		"fr25", "fr26", "fr27", "fr28", "fr29", "fr30", "fr31", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21",
		"v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
	if (depth == 0) {
		test_entry_snapshot(&at_bottom);
		if (trace.wanted) {
			trace.count = 0;
			_Unwind_Backtrace(trace_frame, &trace);
		}
		bottom();
	} else {
		clobber(depth - 1, bottom);
	}
}

void throw_now()
{
	throw 7;
}

void jump_now()
{
	longjmp(test_jump, 1);
}

// The catch sits above the clobbering frames, so the unwinder alone gives back what was in the registers.
void through_throw(void *)
{
	try {
		clobber(DEPTH, throw_now);
	} catch (int) {
		test_entry_snapshot(&at_catch);
	}
}

// The same with nothing in the catch, as in the console run where the registers didn't come back
// (results/2026-10-12-convention-compiler/xsdk_test.log).
void through_throw_plain(void *)
{
	try {
		clobber(DEPTH, throw_now);
	} catch (int) {
	}
}

void clobber_then_jump()
{
	clobber(DEPTH, jump_now);
}

void through_longjmp(void *)
{
	(void)test_run_setjmp(clobber_then_jump);
}

void log_changed(const char *how, EntryChanged changed)
{
	xsdk_log_write("unwind: %s r14-r31 low halves %08X, high halves %08X, f14-f31 %08X, v14-v31 %08X", how,
		(unsigned)changed.gpr_low, (unsigned)changed.gpr_high, (unsigned)changed.fpr, (unsigned)changed.vr);
}

void log_values(const char *where, const EntryRegs &regs)
{
	xsdk_log_write("unwind: %s r14 %016llX, r31 %016llX, f14 %016llX, v14 %08X", where, (unsigned long long)regs.gpr[0],
		(unsigned long long)regs.gpr[ENTRY_REGISTERS - 1], (unsigned long long)regs.fpr[0], (unsigned)regs.vr[0][0]);
}

void log_trace()
{
	for (unsigned i = 0; i < trace.count; i++) {
		const TracedFrame &f = trace.frames[i];
		xsdk_log_write("unwind: frame %u returns to %08X, CFA %08X: r14 %016llX, r31 %016llX, f14 %016llX", i,
			(unsigned)f.ip, (unsigned)f.cfa, (unsigned long long)f.r14, (unsigned long long)f.r31,
			(unsigned long long)f.f14);
	}
}

// Whether the frames in between overwrote every register, and whether they all came back.
void kept_through(const char *name, void (*through)(void *), bool traced)
{
	constexpr uint32_t ALL = ((1u << ENTRY_REGISTERS) - 1) << ENTRY_FIRST_KEPT;
	static EntryRegs after __attribute__((aligned(16)));
	trace.wanted = traced;
	trace.count = 0;
	test_entry_regs_call(through, nullptr, &after);
	const EntryChanged overwritten = test_entry_changed(&at_bottom);
	const EntryChanged returned = test_entry_changed(&after);
	log_changed("the deepest frame had changed", overwritten);
	log_changed("after, its caller found changed", returned);
	if (traced) {
		log_trace();
		log_values("deepest frame:", at_bottom);
		log_values("in the catch:", at_catch);
		log_values("after:", after);
	}
	check(name,
		overwritten.gpr_low == ALL && overwritten.gpr_high == ALL && overwritten.fpr == ALL && overwritten.vr == ALL &&
			!returned.gpr_low && !returned.gpr_high && !returned.fpr && !returned.vr);
}

constexpr unsigned RUNS = 200;

// Runs through RUNS times; returns how many runs lost a register, and the first that did in *first (RUNS if none).
unsigned lost_runs(void (*through)(void *), unsigned *first)
{
	static EntryRegs after __attribute__((aligned(16)));
	unsigned lost = 0;
	*first = RUNS;
	for (unsigned i = 0; i < RUNS; i++) {
		test_entry_regs_call(through, nullptr, &after);
		const EntryChanged changed = test_entry_changed(&after);
		if (changed.gpr_low | changed.gpr_high | changed.fpr | changed.vr) {
			if (!lost)
				*first = i;
			lost++;
		}
	}
	return lost;
}

} // namespace

extern "C" void test_cpp_unwind(void)
{
	// Before any trace, so nothing the unwinder sets up on first use is set up yet by this test.
	unsigned plain_first, caught_first;
	const unsigned plain = lost_runs(through_throw_plain, &plain_first);
	const unsigned caught = lost_runs(through_throw, &caught_first);
	xsdk_log_write(
		"unwind: of %u throws, %u lost registers with an empty catch (first run %u), %u with a catch that "
		"reads them (first run %u)",
		RUNS, plain, plain_first, caught, caught_first);
	check("unwind: every one of 400 throws gives back r14-r31 whole, f14-f31 and v14-v31", !plain && !caught);
	kept_through("unwind: r14-r31 whole, f14-f31 and v14-v31, all overwritten below, come back through a C++ exception",
		through_throw, true);
	kept_through("unwind: the same come back through setjmp and longjmp", through_longjmp, false);
}
