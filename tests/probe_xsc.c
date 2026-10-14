#include <malloc.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "xsdk/log.h"
#include "xsdk/xsc.h"

// xost's compiler in C on the console: every program in Dusklight's pack compiled from its SPIR-V and checked against
// the container, literals and resources the PC made (docs/scope/shader-compiling.md, step 2).

typedef struct {
	uint32_t reg;
	float values[4];
} ProbeLiteral;

typedef struct {
	const char *name;
	const uint32_t *spirv;
	size_t spirv_words;
	const uint32_t *container;
	size_t container_words;
	const ProbeLiteral *literals;
	size_t literal_count;
	const XsdkXscResource *resources;
	size_t resource_count;
} ProbeProgram;

typedef struct {
	const char *name;
	const uint32_t *spirv;
	size_t spirv_words;
	const char *message;
} ProbeRefusal;

#include "probe_xsc_programs.h"

enum {
	PROGRAM_COUNT = sizeof programs / sizeof programs[0],
	REFUSAL_COUNT = sizeof refusals / sizeof refusals[0],
	REGISTERS = 64,
	// The compiling thread's stack; painting leaves its top, which the kernel may use, and a margin below the painter.
	STACK_BYTES = 1024 * 1024,
	STACK_TOP_MARGIN = 64 * 1024,
	PAINT_MARGIN = 4 * 1024,
	PAINT = 0x5A17C0DE,
	NANOSECONDS_PER_MICROSECOND = 1000,
	MICROSECONDS_PER_SECOND = 1000000,
};

typedef struct {
	unsigned same;
	unsigned refused_right;
	uint64_t total_us;
	uint64_t slowest_us;
	const char *slowest;
	size_t heap_most;
	size_t arena_most;
	size_t stack_most;
	size_t painted;
} Results;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * MICROSECONDS_PER_SECOND + (uint64_t)now.tv_nsec / NANOSECONDS_PER_MICROSECOND;
}

static void options(XsdkXscOptions *o)
{
	xsdk_xsc_default_options(o);
	o->pixel_registers = REGISTERS;
	o->vertex_registers = REGISTERS;
	o->optimize = pack_optimized;
	// Broken on purpose: the fetch layout off, so pixel shaders with fetches don't match the pack.
#ifdef XSC_PROBE_BREAK
	o->group_fetches = false;
#endif
}

// What differs from the pack, or NULL where nothing does.
static const char *difference(const ProbeProgram *p, const XsdkXscShader *s)
{
	if (s->container_bytes != p->container_words * sizeof *p->container ||
		memcmp(s->container, p->container, s->container_bytes) != 0)
		return "container";
	if (s->need_count != p->literal_count)
		return "literal count";
	for (size_t i = 0; i < s->need_count; i++) {
		const XsdkXscNeed *const n = &s->needs[i];
		if (n->kind != XSDK_XSC_LITERAL || n->reg != p->literals[i].reg)
			return "literal register";
		for (int c = 0; c < 4; c++) {
			if ((float)n->value[c] != p->literals[i].values[c])
				return "literal value";
		}
	}
	if (s->resource_count != p->resource_count)
		return "resource count";
	for (size_t i = 0; i < s->resource_count; i++) {
		const XsdkXscResource *const a = &s->resources[i];
		const XsdkXscResource *const b = &p->resources[i];
		if (a->kind != b->kind || a->set != b->set || a->binding != b->binding || a->reg != b->reg ||
			a->regs != b->regs)
			return "resource";
	}
	return NULL;
}

static void note_heap(Results *r)
{
	const struct mallinfo heap = mallinfo();
	if ((size_t)heap.uordblks > r->heap_most)
		r->heap_most = (size_t)heap.uordblks;
	if ((size_t)heap.arena > r->arena_most)
		r->arena_most = (size_t)heap.arena;
}

static void compile_program(const ProbeProgram *p, const XsdkXscOptions *o, Results *r)
{
	XsdkXscShader shader;
	const uint64_t start = now_us();
	const XsdkXscStatus status = xsdk_xsc_compile(p->spirv, p->spirv_words, o, &shader);
	const uint64_t us = now_us() - start;
	note_heap(r);
	const char *const differs = status == XSDK_XSC_OK ? difference(p, &shader) : "status";
	r->same += !differs;
	r->total_us += us;
	if (us > r->slowest_us) {
		r->slowest_us = us;
		r->slowest = p->name;
	}
	if (status != XSDK_XSC_OK)
		xsdk_log_write("xsc: %s: %llu us, FAILED %d: %s", p->name, (unsigned long long)us, status, shader.message);
	else
		xsdk_log_write("xsc: %s: %llu us, %u bytes, %s", p->name, (unsigned long long)us,
			(unsigned)shader.container_bytes, differs ? differs : "the pack's");
	if (differs && status == XSDK_XSC_OK)
		xsdk_log_write("xsc: %s: DIFFERS from the pack in its %s", p->name, differs);
	xsdk_xsc_free(&shader);
}

static void compile_refusal(const ProbeRefusal *p, const XsdkXscOptions *o, Results *r)
{
	XsdkXscShader shader;
	const XsdkXscStatus status = xsdk_xsc_compile(p->spirv, p->spirv_words, o, &shader);
	const bool right = status == XSDK_XSC_REFUSED && strcmp(shader.message, p->message) == 0;
	r->refused_right += right;
	xsdk_log_write("xsc: %s: status %d, \"%s\": %s", p->name, status, shader.message,
		right ? "refused as on the PC" : "NOT refused as on the PC");
	xsdk_xsc_free(&shader);
}

// Paints the stack below this function's frame down to bottom, so what compiling writes there shows.
static __attribute__((noinline)) void paint_stack(uintptr_t bottom)
{
	const uintptr_t here = (uintptr_t)__builtin_frame_address(0) - PAINT_MARGIN;
	for (volatile uint32_t *word = (volatile uint32_t *)bottom; (uintptr_t)word < here; word++)
		*word = PAINT;
}

static void *compile_all(void *argument)
{
	Results *const r = argument;
	const uintptr_t top = (uintptr_t)__builtin_frame_address(0);
	const uintptr_t bottom = top - STACK_BYTES + STACK_TOP_MARGIN;
	paint_stack(bottom);
	r->painted = top - PAINT_MARGIN - bottom;
	XsdkXscOptions o;
	options(&o);
	for (size_t i = 0; i < PROGRAM_COUNT; i++)
		compile_program(&programs[i], &o, r);
	for (size_t i = 0; i < REFUSAL_COUNT; i++)
		compile_refusal(&refusals[i], &o, r);
	const volatile uint32_t *word = (const volatile uint32_t *)bottom;
	while ((uintptr_t)word < top - PAINT_MARGIN && *word == PAINT)
		word++;
	r->stack_most = top - (uintptr_t)word;
	return NULL;
}

int main(void)
{
#ifdef XSC_PROBE_BREAK
	xsdk_log_write("xsc probe: %u programs, the fetch layout off (broken on purpose)", (unsigned)PROGRAM_COUNT);
#else
	xsdk_log_write("xsc probe: %u programs", (unsigned)PROGRAM_COUNT);
#endif
	Results r = { 0 };
	const size_t heap_before = (size_t)mallinfo().uordblks;
	pthread_attr_t attributes;
	pthread_t thread;
	if (pthread_attr_init(&attributes) != 0 || pthread_attr_setstacksize(&attributes, STACK_BYTES) != 0 ||
		pthread_create(&thread, &attributes, compile_all, &r) != 0) {
		xsdk_log_write("xsc probe: FAIL, the compiling thread wasn't made");
		return 1;
	}
	(void)pthread_join(thread, NULL);
	(void)pthread_attr_destroy(&attributes);
	const unsigned count = PROGRAM_COUNT;
	xsdk_log_write("xsc probe: %u of %u programs the pack's, %u of %u refusals right", r.same, count, r.refused_right,
		(unsigned)REFUSAL_COUNT);
	xsdk_log_write("xsc probe: %llu us in all, %llu us on average, the slowest %llu us (%s)",
		(unsigned long long)r.total_us, (unsigned long long)(r.total_us / count), (unsigned long long)r.slowest_us,
		r.slowest ? r.slowest : "none");
	// The heap's high-water mark bounds what compiling takes, as the arena frees each compile's memory at its end.
	xsdk_log_write("xsc probe: heap %u bytes in use before, grown to %u at most, %u in use after a compile at most",
		(unsigned)heap_before, (unsigned)r.arena_most, (unsigned)r.heap_most);
	xsdk_log_write("xsc probe: stack %u bytes at most of %u", (unsigned)r.stack_most, (unsigned)r.painted);
	const bool passed = r.same == count && r.refused_right == REFUSAL_COUNT;
	xsdk_log_write("xsc probe: %s", passed ? "PASS" : "FAIL");
	return passed ? 0 : 1;
}
