#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bar_scene.h"
#include "probe.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"

// Milestone 40's probe (docs/scope/40-fault-handler.md): what the console does with a CPU exception in a title, and how
// it calls a handler set with RtlSetVectoredExceptionHandler. probe_fault.txt beside the .xex picks the mode:
// - plain: no handler; a store to address 0, then whatever follows;
// - probe, the default: the handler, the context's layout, then each fault in turn, resumed;
// - overflow: the handler, and a thread that overflows its stack;
// - plain_overflow: the same overflow with no handler;
// - end: the handler ends the title itself, with the GPU and the display open.

enum {
	// Neither has code in Xenia (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc:891-892).
	ORDINAL_SET_HANDLER = 0x36D,
	ORDINAL_CLEAR_HANDLER = 0x36E,
	// Windows' codes (Microsoft's ntstatus.h), which the console's other status codes follow.
	CODE_ACCESS_VIOLATION = 0xC0000005,
	CODE_MISALIGNMENT = 0x80000002,
	CODE_ILLEGAL_INSTRUCTION = 0xC000001D,
	CODE_BREAKPOINT = 0x80000003,
	// Windows' vectored handler results: resume at the context, or pass the exception on.
	CONTINUE_EXECUTION = -1,
	CONTINUE_SEARCH = 0,
	// Windows' EXCEPTION_RECORD, as Xenia lays it out (xenia:src/xenia/xbox.h:314-323).
	RECORD_BYTES = 0x50,
	RECORD_WORDS = RECORD_BYTES / 4,
	// How far past the context the layout search reads, and how much of it the log shows.
	CONTEXT_SEARCH_BYTES = 0x1000,
	CONTEXT_DUMP_BYTES = 0x300,
	DUMP_WORDS_PER_LINE = 8,
	// Where the processor control region keeps the stack's high and low ends (xenia:src/xenia/kernel/xthread.h:78-79).
	PCR_STACK_BASE = 0x70,
	PCR_STACK_END = 0x74,
	WALK_FRAMES = 6,
	GPR_PATTERNS = 32,
	FPR_PATTERNS = 18,
	FIRST_SAVED_REGISTER = 14,
	AFTER_WORDS = 2 * (18 + 18),
	MODE_BYTES = 32,
	RUN_ON_MS = 5000,
	FRAMES = 60,
	FAULT_FRAME = 30,
	BUFFERS = 2,
	SWAP_INTERVAL = 1,
	BAR_ARGB = 0xFFFFFFFF,
	BEGIN_TIMEOUT_MS = 1000,
	WC_BYTES = 4096,
	OVERFLOW_STACK_BYTES = 64 * 1024,
	OVERFLOW_FRAME_BYTES = 1024,
	OVERFLOW_LOG_EVERY = 8,
	THREAD_WAIT_MS = 5000,
	OVERFLOW_DEPTH_LIMIT = 1000000,
	NOT_FOUND = UINT32_MAX,
};

typedef uint32_t (*SetHandler)(uint32_t first, uint32_t handler);
typedef uint32_t (*ClearHandler)(uint32_t handle);
typedef void (*FaultFunction)(uint32_t argument);

// tests/probe_fault_sites.S
extern const uint32_t probe_fault_patterns[];
extern uint32_t probe_fault_patterned_lr;
extern uint32_t probe_fault_patterned_r1;
void probe_fault_patterned_store(uint32_t address, uint32_t *after);
void probe_fault_store(uint32_t address);
void probe_fault_load(uint32_t address);
void probe_fault_store_doubleword(uint32_t address);
void probe_fault_illegal(uint32_t unused);
void probe_fault_trap(uint32_t unused);
extern const uint32_t probe_fault_patterned_site[];
extern const uint32_t probe_fault_store_site[];
extern const uint32_t probe_fault_load_site[];
extern const uint32_t probe_fault_store_doubleword_site[];
extern const uint32_t probe_fault_illegal_site[];
extern const uint32_t probe_fault_trap_site[];

typedef struct {
	uint32_t calls;
	uint32_t arguments[4];
	uint32_t hardware_thread;
	uint32_t record[RECORD_WORDS];
	uint32_t context;
	uint32_t walk[WALK_FRAMES];
} Seen;

// What the handler saw last; it runs on the faulting thread, and the probe reads it after a signal fence.
static Seen seen;
// The context's offsets the patterned store finds, NOT_FOUND until then: the word holding IAR's low half, and r1's.
static uint32_t iar_offset = NOT_FOUND;
static uint32_t r1_offset = NOT_FOUND;
static bool discover;
static bool pass_on;
static bool end_in_handler;
static uint32_t in_handler;
static uint32_t passed_count;
static uint32_t failed_count;
// Counts main's runs, as a fault with no handler once ran main again in the same image.
static uint32_t main_runs;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static uint32_t read_word(uint32_t address)
{
	return *(volatile const uint32_t *)address;
}

// Whether bytes at address lie on the calling thread's stack, the one place the handler reads without knowing more.
static bool on_stack(uint32_t address, uint32_t bytes)
{
	uint32_t pcr;
	__asm__("mr %0, 13" : "=r"(pcr));
	const uint32_t high = read_word(pcr + PCR_STACK_BASE);
	const uint32_t low = read_word(pcr + PCR_STACK_END);
	return address >= low && address < high && bytes <= high - address;
}

static uint32_t stack_bytes_above(uint32_t address)
{
	uint32_t pcr;
	__asm__("mr %0, 13" : "=r"(pcr));
	const uint32_t high = read_word(pcr + PCR_STACK_BASE);
	return address < high ? high - address : 0;
}

// Windows passes a pointer to { record, context }; the other likely form passes both directly.
static bool find_record(uint32_t first, uint32_t second, uint32_t *record, uint32_t *context)
{
	if (on_stack(first, 8)) {
		const uint32_t pointed_record = read_word(first);
		const uint32_t pointed_context = read_word(first + 4);
		if (on_stack(pointed_record, RECORD_BYTES) && on_stack(pointed_context, 4)) {
			xsdk_log_write("fault: the first argument points to { record %08X, context %08X }",
				(unsigned)pointed_record, (unsigned)pointed_context);
			*record = pointed_record;
			*context = pointed_context;
			return true;
		}
	}
	if (on_stack(first, RECORD_BYTES) && on_stack(second, 4)) {
		xsdk_log_write(
			"fault: the arguments are the record %08X and the context %08X", (unsigned)first, (unsigned)second);
		*record = first;
		*context = second;
		return true;
	}
	xsdk_log_write("fault: neither argument form fits, so the handler reads nothing");
	return false;
}

static void log_record(uint32_t record)
{
	for (uint32_t i = 0; i < RECORD_WORDS; i++)
		seen.record[i] = read_word(record + 4 * i);
	xsdk_log_write("fault: code %08X, flags %08X, nested record %08X, address %08X, %u parameters: %08X %08X %08X",
		(unsigned)seen.record[0], (unsigned)seen.record[1], (unsigned)seen.record[2], (unsigned)seen.record[3],
		(unsigned)seen.record[4], (unsigned)seen.record[5], (unsigned)seen.record[6], (unsigned)seen.record[7]);
}

static void dump_context(uint32_t context, uint32_t bytes)
{
	for (uint32_t offset = 0; offset < bytes; offset += 4 * DUMP_WORDS_PER_LINE) {
		uint32_t words[DUMP_WORDS_PER_LINE];
		for (uint32_t i = 0; i < DUMP_WORDS_PER_LINE; i++)
			words[i] = read_word(context + offset + 4 * i);
		xsdk_log_write("fault: context +0x%03X: %08X %08X %08X %08X %08X %08X %08X %08X", (unsigned)offset,
			(unsigned)words[0], (unsigned)words[1], (unsigned)words[2], (unsigned)words[3], (unsigned)words[4],
			(unsigned)words[5], (unsigned)words[6], (unsigned)words[7]);
	}
}

// The record's place, which the searches skip, as it holds the faulting address too.
static uint32_t record_at;

static bool in_record(uint32_t address)
{
	return address >= record_at && address < record_at + RECORD_BYTES;
}

// Every 4-byte offset in the context holding value, logged; gives the first, the context's own, or NOT_FOUND.
static uint32_t find_word(const char *name, uint32_t context, uint32_t bytes, uint32_t value, uint32_t *count)
{
	uint32_t first = NOT_FOUND;
	*count = 0;
	for (uint32_t offset = 0; offset + 4 <= bytes; offset += 4) {
		if (in_record(context + offset) || read_word(context + offset) != value)
			continue;
		if (*count == 0)
			first = offset;
		(*count)++;
		if (*count <= 4)
			xsdk_log_write("fault: %s (%08X) at +0x%03X", name, (unsigned)value, (unsigned)offset);
	}
	if (*count == 0)
		xsdk_log_write("fault: %s (%08X) not found", name, (unsigned)value);
	else if (*count > 4)
		xsdk_log_write("fault: %s found %u times", name, (unsigned)*count);
	return first;
}

// A 64-bit pattern: whole, or its low word alone where the context keeps 32 bits.
static void find_pattern(const char *name, uint32_t context, uint32_t bytes, const uint32_t *pattern)
{
	for (uint32_t offset = 0; offset + 8 <= bytes; offset += 4) {
		if (!in_record(context + offset) && read_word(context + offset) == pattern[0] &&
			read_word(context + offset + 4) == pattern[1]) {
			xsdk_log_write("fault: %s at +0x%03X, 64 bits", name, (unsigned)offset);
			return;
		}
	}
	uint32_t count;
	(void)find_word(name, context, bytes, pattern[1], &count);
}

static void discover_layout(uint32_t context)
{
	const uint32_t above = stack_bytes_above(context);
	const uint32_t bytes = above < CONTEXT_SEARCH_BYTES ? above : CONTEXT_SEARCH_BYTES;
	xsdk_log_write("fault: the context at %08X, %u bytes searched", (unsigned)context, (unsigned)bytes);
	dump_context(context, bytes < CONTEXT_DUMP_BYTES ? bytes : CONTEXT_DUMP_BYTES);
	char name[16];
	for (uint32_t n = 0; n < GPR_PATTERNS; n++) {
		// r1-r4 and r13 hold the frame, the arguments and the PCR, not patterns.
		if ((n >= 1 && n <= 4) || n == 13)
			continue;
		snprintf(name, sizeof name, "r%u", (unsigned)n);
		find_pattern(name, context, bytes, &probe_fault_patterns[2 * n]);
	}
	for (uint32_t n = 0; n < FPR_PATTERNS; n++) {
		snprintf(name, sizeof name, "f%u", (unsigned)(FIRST_SAVED_REGISTER + n));
		find_pattern(name, context, bytes, &probe_fault_patterns[2 * (GPR_PATTERNS + n)]);
	}
	const uint32_t *specials = &probe_fault_patterns[2 * (GPR_PATTERNS + FPR_PATTERNS)];
	find_pattern("CTR", context, bytes, specials);
	uint32_t count;
	(void)find_word("CR", context, bytes, specials[2], &count);
	(void)find_word("XER", context, bytes, specials[3], &count);
	(void)find_word("LR", context, bytes, probe_fault_patterned_lr, &count);
	uint32_t pcr;
	__asm__("mr %0, 13" : "=r"(pcr));
	(void)find_word("r13", context, bytes, pcr, &count);
	// The faulting frame above the context holds some of these too, so the first match is the context's.
	r1_offset = find_word("r1", context, bytes, probe_fault_patterned_r1, &count);
	iar_offset = find_word("IAR", context, bytes, (uint32_t)probe_fault_patterned_site, &count);
}

// The back chain from the faulting r1: each frame's first word, and what the SysV slot 4 bytes into the next frame and
// MSVC's slot 8 bytes below it hold.
static void walk_stack(uint32_t context)
{
	if (r1_offset == NOT_FOUND)
		return;
	uint32_t frame = read_word(context + r1_offset);
	for (uint32_t i = 0; i < WALK_FRAMES && on_stack(frame, 4); i++) {
		const uint32_t next = read_word(frame);
		const bool readable = on_stack(next - 8, 16);
		const uint32_t sysv = readable ? read_word(next + 4) : 0;
		const uint32_t msvc = readable ? read_word(next - 8) : 0;
		seen.walk[i] = sysv;
		xsdk_log_write("fault: frame %u at %08X, next %08X, +4 holds %08X, -8 holds %08X", (unsigned)i, (unsigned)frame,
			(unsigned)next, (unsigned)sysv, (unsigned)msvc);
		frame = next;
	}
}

static int32_t handler(uint32_t first, uint32_t second, uint32_t third, uint32_t fourth)
{
	// A fault inside the handler passes on rather than loops.
	if (__atomic_exchange_n(&in_handler, 1, __ATOMIC_ACQUIRE))
		return CONTINUE_SEARCH;
	seen.calls++;
	seen.arguments[0] = first;
	seen.arguments[1] = second;
	seen.arguments[2] = third;
	seen.arguments[3] = fourth;
	seen.hardware_thread = xsdk_thread_hardware_thread();
	xsdk_log_write("fault: handler call %u on hardware thread %u, arguments %08X %08X %08X %08X", (unsigned)seen.calls,
		(unsigned)seen.hardware_thread, (unsigned)first, (unsigned)second, (unsigned)third, (unsigned)fourth);
	int32_t result = CONTINUE_SEARCH;
	uint32_t record;
	uint32_t context;
	if (find_record(first, second, &record, &context)) {
		seen.context = context;
		record_at = record;
		log_record(record);
		if (discover) {
			discover_layout(context);
			walk_stack(context);
		}
		if (end_in_handler) {
			xsdk_log_write("end: XamLoaderTerminateTitle from the handler, the GPU and the display still open");
			XamLoaderTerminateTitle();
			xsdk_log_write("end: XamLoaderTerminateTitle returned");
		}
		if (!pass_on && iar_offset != NOT_FOUND) {
			const uint32_t iar = read_word(context + iar_offset);
			*(volatile uint32_t *)(context + iar_offset) = iar + 4;
			result = CONTINUE_EXECUTION;
		}
	}
	xsdk_log_write("fault: handler returns %d", (int)result);
	__atomic_store_n(&in_handler, 0, __ATOMIC_RELEASE);
	return result;
}

static bool resolve(uint32_t ordinal, void **slot)
{
	HANDLE kernel = 0;
	const uint32_t error = XexGetModuleHandle("xboxkrnl.exe", &kernel);
	if (error != 0) {
		xsdk_log_write("fault: XexGetModuleHandle(xboxkrnl.exe) failed: %08X", (unsigned)error);
		return false;
	}
	if (XexGetProcedureAddress(kernel, ordinal, slot) < 0 || !*slot) {
		xsdk_log_write("fault: xboxkrnl.exe has no ordinal 0x%03X", (unsigned)ordinal);
		return false;
	}
	return true;
}

// Sets the handler, passed as both arguments so that either (first, handler) or (handler) takes it.
static uint32_t set_handler(void)
{
	SetHandler set;
	if (!resolve(ORDINAL_SET_HANDLER, (void **)&set))
		return 0;
	const uint32_t address = (uint32_t)handler;
	const uint32_t result = set(address, address);
	xsdk_log_write("fault: RtlSetVectoredExceptionHandler(%08X, %08X) returned %08X", (unsigned)address,
		(unsigned)address, (unsigned)result);
	return result;
}

static void read_mode(char *mode)
{
	strcpy(mode, "probe");
	FILE *file = fopen("probe_fault.txt", "r");
	if (!file)
		return;
	char line[MODE_BYTES] = "";
	if (fgets(line, sizeof line, file))
		line[strcspn(line, "\r\n ")] = '\0';
	fclose(file);
	if (line[0])
		strcpy(mode, line);
}

// Logs a line a second for a while, to show whether the title carries on.
static void run_on(const char *mode)
{
	for (uint32_t ms = 0; ms < RUN_ON_MS; ms += 1000) {
		xsdk_log_write("%s: still running, %u ms", mode, (unsigned)ms);
		(void)xsdk_thread_sleep(1000);
	}
}

static void fence(void)
{
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
}

static bool caught(const char *name, uint32_t calls_before, uint32_t site, uint32_t code)
{
	fence();
	const bool called = seen.calls == calls_before + 1;
	xsdk_log_write("fault: %s: %s, code %08X (Windows says %08X), address %08X (the site %08X)", name,
		called ? "handled" : "not handled", (unsigned)seen.record[0], (unsigned)code, (unsigned)seen.record[3],
		(unsigned)site);
	return called && seen.record[3] == site;
}

static void fault_and_resume(
	const char *name, FaultFunction function, uint32_t argument, const uint32_t *site, uint32_t code)
{
	xsdk_log_write("fault: %s, from %08X", name, (unsigned)site);
	const uint32_t calls = seen.calls;
	function(argument);
	check(name, caught(name, calls, (uint32_t)site, code));
}

static uint32_t patterned_return;

__attribute__((noinline)) static void patterned_store(void)
{
	patterned_return = (uint32_t)__builtin_return_address(0);
	uint32_t after[AFTER_WORDS] __attribute__((aligned(8))) = { 0 };
	const uint32_t calls = seen.calls;
	discover = true;
	probe_fault_patterned_store(0, after);
	discover = false;
	check("fault: the patterned store resumed past its site",
		caught("patterned store", calls, (uint32_t)probe_fault_patterned_site, CODE_ACCESS_VIOLATION));
	const uint32_t *words = after;
	bool whole = true;
	for (uint32_t n = 0; n < 18; n++) {
		const uint32_t *gpr = &probe_fault_patterns[2 * (FIRST_SAVED_REGISTER + n)];
		const uint32_t *fpr = &probe_fault_patterns[2 * (GPR_PATTERNS + n)];
		whole = whole && words[2 * n] == gpr[0] && words[2 * n + 1] == gpr[1];
		whole = whole && words[36 + 2 * n] == fpr[0] && words[36 + 2 * n + 1] == fpr[1];
	}
	check("fault: r14-r31 and f14-f31 came back whole after the resume", whole);
	xsdk_log_write("fault: the walk's return addresses %08X %08X, expected %08X %08X", (unsigned)seen.walk[0],
		(unsigned)seen.walk[1], (unsigned)probe_fault_patterned_lr, (unsigned)patterned_return);
	check("fault: the back chain's SysV slots hold the two return addresses",
		seen.walk[0] == probe_fault_patterned_lr && seen.walk[1] == patterned_return);
}

static uint32_t thread_fault(void *context)
{
	(void)context;
	probe_fault_store(0);
	return xsdk_thread_hardware_thread();
}

static void fault_on_each_hardware_thread(void)
{
	for (uint32_t hardware_thread = 0; hardware_thread < XSDK_HARDWARE_THREADS; hardware_thread++) {
		const uint32_t calls = seen.calls;
		HANDLE thread;
		const NTSTATUS status = xsdk_thread_create_on(&thread, 0, thread_fault, NULL, hardware_thread);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write(
				"fault: xsdk_thread_create_on(%u) failed: %08X", (unsigned)hardware_thread, (unsigned)status);
			check("fault: a store to 0 on each hardware thread", false);
			return;
		}
		const int64_t timeout = -(int64_t)THREAD_WAIT_MS * 10000;
		const NTSTATUS waited = NtWaitForSingleObjectEx(thread, USER_MODE, 0, &timeout);
		(void)NtClose(thread);
		fence();
		char name[64];
		snprintf(
			name, sizeof name, "fault: a store to 0 on hardware thread %u, handled there", (unsigned)hardware_thread);
		check(name,
			waited == STATUS_SUCCESS && seen.calls == calls + 1 && seen.hardware_thread == hardware_thread &&
				seen.record[3] == (uint32_t)probe_fault_store_site);
	}
}

static bool open_presenting(BarScene *scene)
{
	uint32_t width = 0;
	uint32_t height = 0;
	return NT_SUCCESS(xsdk_gpu_open()) && bar_scene_open(scene, BAR_ARGB) &&
		NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height));
}

// Presents FRAMES frames, with a store to 0 before FAULT_FRAME; true if every frame went out.
static bool present_with_fault(BarScene *scene)
{
	for (uint32_t frame = 0; frame < FRAMES; frame++) {
		uint32_t address;
		if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS)))
			return false;
		if (frame == FAULT_FRAME) {
			xsdk_log_write("fault: a store to 0 at frame %u, presenting", (unsigned)frame);
			probe_fault_store(0);
		}
		bar_scene_draw(scene, frame, address);
		xsdk_present_end();
	}
	return true;
}

static void fault_while_presenting(void)
{
	BarScene scene;
	if (!open_presenting(&scene)) {
		check("fault: GPU, scene and present open", false);
		return;
	}
	const uint32_t calls = seen.calls;
	const bool presented = present_with_fault(&scene);
	xsdk_present_close();
	XsdkPresentStats stats;
	xsdk_present_stats(&stats);
	bar_scene_close(&scene);
	const bool closed = NT_SUCCESS(xsdk_gpu_close());
	fence();
	xsdk_log_write(
		"fault: presenting: %u flips, %u handler calls", (unsigned)stats.flips, (unsigned)(seen.calls - calls));
	check("fault: a store to 0 while presenting, handled, every frame shown and the GPU closed",
		presented && closed && stats.flips == FRAMES && seen.calls == calls + 1);
}

static void probe_mode(void)
{
	const uint32_t handle = set_handler();
	patterned_store();
	if (iar_offset == NOT_FOUND) {
		xsdk_log_write("fault: IAR wasn't found in the context, so nothing more can resume");
		return;
	}
	fault_and_resume("a load from 0", probe_fault_load, 0, probe_fault_load_site, CODE_ACCESS_VIOLATION);
	fault_and_resume("a store to 0", probe_fault_store, 0, probe_fault_store_site, CODE_ACCESS_VIOLATION);
	volatile uint32_t *memory = xsdk_gpu_alloc(WC_BYTES);
	if (memory) {
		fault_and_resume("a misaligned 64-bit store into write-combined memory", probe_fault_store_doubleword,
			(uint32_t)memory + 4, probe_fault_store_doubleword_site, CODE_MISALIGNMENT);
		xsdk_gpu_free(memory);
	}
	fault_and_resume(
		"an illegal instruction", probe_fault_illegal, 0, probe_fault_illegal_site, CODE_ILLEGAL_INSTRUCTION);
	fault_and_resume("a trap", probe_fault_trap, 0, probe_fault_trap_site, CODE_BREAKPOINT);
	fault_on_each_hardware_thread();
	fault_while_presenting();
	xsdk_log_write("probe: %u passed, %u failed", (unsigned)passed_count, (unsigned)failed_count);

	ClearHandler clear;
	if (resolve(ORDINAL_CLEAR_HANDLER, (void **)&clear)) {
		const uint32_t result = clear(handle);
		xsdk_log_write(
			"fault: RtlClearVectoredExceptionHandler(%08X) returned %08X", (unsigned)handle, (unsigned)result);
	}
	// In case the handler is still set, it passes the last fault on.
	pass_on = true;
	const uint32_t calls = seen.calls;
	xsdk_log_write("probe: a store to 0 after clearing the handler");
	probe_fault_store(0);
	fence();
	xsdk_log_write(
		"probe: carried on past the store, the handler called %u more times", (unsigned)(seen.calls - calls));
	run_on("probe");
}

// Recurses with a kilobyte of stack a call until the stack runs out.
__attribute__((noinline)) static uint32_t recurse(uint32_t depth)
{
	volatile uint8_t frame[OVERFLOW_FRAME_BYTES];
	frame[0] = (uint8_t)depth;
	if (depth > OVERFLOW_DEPTH_LIMIT)
		return 0;
	if (depth % OVERFLOW_LOG_EVERY == 0) {
		uint32_t sp;
		__asm__("mr %0, 1" : "=r"(sp));
		xsdk_log_write("overflow: depth %u, r1 %08X", (unsigned)depth, (unsigned)sp);
	}
	return recurse(depth + 1) + frame[0];
}

static void *overflow_thread(void *argument)
{
	(void)argument;
	uint32_t pcr;
	__asm__("mr %0, 13" : "=r"(pcr));
	xsdk_log_write("overflow: stack %08X to %08X", (unsigned)read_word(pcr + PCR_STACK_END),
		(unsigned)read_word(pcr + PCR_STACK_BASE));
	return (void *)(uintptr_t)recurse(0);
}

static void overflow_mode(bool with_handler)
{
	if (with_handler)
		(void)set_handler();
	pass_on = true;
	pthread_attr_t attributes;
	pthread_t thread;
	(void)pthread_attr_init(&attributes);
	(void)pthread_attr_setstacksize(&attributes, OVERFLOW_STACK_BYTES);
	if (pthread_create(&thread, &attributes, overflow_thread, NULL) != 0) {
		xsdk_log_write("overflow: pthread_create failed");
		return;
	}
	(void)pthread_join(thread, NULL);
	xsdk_log_write("overflow: the thread ended");
	run_on("overflow");
}

static void end_mode(void)
{
	(void)set_handler();
	pass_on = true;
	end_in_handler = true;
	BarScene scene;
	if (!open_presenting(&scene)) {
		xsdk_log_write("end: GPU, scene or present didn't open");
		return;
	}
	(void)present_with_fault(&scene);
	xsdk_log_write("end: carried on past the fault");
	run_on("end");
}

static void plain_mode(void)
{
	xsdk_log_write("plain: a store to 0, no handler");
	probe_fault_store(0);
	xsdk_log_write("plain: carried on past the store");
	run_on("plain");
}

int main(void)
{
	main_runs++;
	char mode[MODE_BYTES];
	read_mode(mode);
	xsdk_log_write("probe_fault: run %u of main, mode %s; sites: patterned %08X, store %08X, load %08X",
		(unsigned)main_runs, mode, (unsigned)probe_fault_patterned_site, (unsigned)probe_fault_store_site,
		(unsigned)probe_fault_load_site);
	if (strcmp(mode, "plain") == 0)
		plain_mode();
	else if (strcmp(mode, "overflow") == 0)
		overflow_mode(true);
	else if (strcmp(mode, "plain_overflow") == 0)
		overflow_mode(false);
	else if (strcmp(mode, "end") == 0)
		end_mode();
	else
		probe_mode();
	xsdk_log_write("probe_fault: main returns");
	return 0;
}
