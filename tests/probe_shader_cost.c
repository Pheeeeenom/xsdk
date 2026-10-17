#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"
#include "xsdk/xsc.h"

// What xost's code costs the GPU (docs/scope/45-xost-code-quality.md, step 1): each program in Dusklight's pack,
// compiled here, timed by the GPU's clock; a pixel shader over 1280x720, its picture hashed from fixed inputs, and a
// vertex shader over vertices all alike, so its triangles have no area and nothing reaches the pixels.

typedef struct {
	const char *name;
	bool pixel;
	const uint32_t *spirv;
	size_t spirv_words;
} ProbeProgram;

#include "probe_shader_cost_programs.h"

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	ROUNDS = 3,
	DRAWS = 4,
	// The most a draw takes: a vertex count is 16 bits (xenia:src/xenia/gpu/registers.h:300-327), in whole triangles.
	DRAW_VERTICES = 65535,
	QUAD_VERTICES = 6,
	VERTEX_BYTES = 16,
	// The probe's own vertex shader reads c0 to c31 (tools/gen_shader_cost_probe.py).
	INTERPOLANT_CONSTANTS = 32,
	TEXTURE_SIZE = 64,
	TEXTURE_SLOTS = 32,
	MAX_ELEMENTS = 16,
	CLEAR_ARGB = 0xFF202020,
	FENCE_TIMEOUT_MS = 2000,
	// The GPU's clock counts 498.75 a microsecond (docs/HARDWARE.md, "Queries and events").
	GPU_CLOCKS_PER_MS = 498750,
	VISIBILITY_ID = 1,
	IDS_PER_WORD = 32,
	// What the query's word holds till its copy lands.
	UNWRITTEN = 0xA5A5A5A5,
	// Copied out of write-combined memory a piece at a time, to hash from the cache.
	HASH_CHUNK_BYTES = 64 * 1024,
	FNV_OFFSET = 2166136261u,
	FNV_PRIME = 16777619u,
};

// A patch record's usage and index (libxenon:drivers/xenos/xe.c:821-823), and SQ_PROGRAM_CNTL's register counts less
// one, vertex then pixel (xenia:src/xenia/gpu/registers.h:134-136).
enum {
	PATCH_USAGE_SHIFT = 12,
	PATCH_INDEX_SHIFT = 16,
	PATCH_FIELD_MASK = 0xF,
	VS_REGISTERS_MASK = 0x3F,
	PS_REGISTERS_SHIFT = 8,
	PS_REGISTERS_MASK = 0x3F,
};

// Control flow instructions, two in three dwords at the microcode's start (xenia:src/xenia/gpu/ucode.h:520-528), and an
// exec's address, count and sequence, two bits an instruction, the low one set for a fetch (ucode.h:239-259).
enum {
	SLOT_DWORDS = 3,
	CF_OPCODE_SHIFT = 12,
	CF_OPCODE_MASK = 0xF,
	EXEC_ADDRESS_MASK = 0xFFF,
	EXEC_COUNT_SHIFT = 12,
	EXEC_COUNT_MASK = 0x7,
	EXEC_SEQUENCE_SHIFT = 16,
	// kExec to kCondExecPredEnd, and kCondExecPredClean and its end (ucode.h:96-162).
	CF_EXEC_FIRST = 1,
	CF_EXEC_LAST = 6,
	CF_EXEC_PRED_CLEAN = 13,
	CF_EXEC_PRED_CLEAN_END = 14,
};

enum { PROGRAM_COUNT = sizeof programs / sizeof programs[0] };

// Each component of every vertex: none zero, none alike, the same for every vertex.
static const float vertex_value[4] = { 0.375f, 0.625f, 0.8125f, 1.0f };

// Two triangles over the target, D3D's clip space.
static const float quad[QUAD_VERTICES][4] = {
	{ -1.0f, 1.0f, 0.5f, 1.0f },
	{ 1.0f, 1.0f, 0.5f, 1.0f },
	{ -1.0f, -1.0f, 0.5f, 1.0f },
	{ -1.0f, -1.0f, 0.5f, 1.0f },
	{ 1.0f, 1.0f, 0.5f, 1.0f },
	{ 1.0f, -1.0f, 0.5f, 1.0f },
};

typedef struct {
	uint32_t timed;
	uint32_t alu;
	uint32_t registers;
	double us;
} Totals;

static XsdkShader probe_vertex;
static XsdkShader probe_pixel;
static XsdkTexture texture;
static XsdkSamplerState sampler;
static volatile uint32_t *quad_vertices;
static volatile uint32_t *mesh_vertices;
static volatile uint32_t *image;
static uint32_t image_bytes;
static volatile uint32_t *clocks;
static volatile uint32_t *visibility;
static float constants[XSDK_FLOAT_CONSTANTS][4];
static uint64_t hash_us;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static volatile uint32_t *gpu_words(const float (*values)[4], uint32_t vertex_count)
{
	volatile uint32_t *const memory = xsdk_gpu_alloc(vertex_count * VERTEX_BYTES);
	if (!memory)
		return NULL;
	for (uint32_t v = 0; v < vertex_count; v++) {
		uint32_t words[4];
		memcpy(words, values ? values[v] : vertex_value, sizeof words);
		for (uint32_t i = 0; i < 4; i++)
			memory[v * 4 + i] = words[i];
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), vertex_count * VERTEX_BYTES);
	return memory;
}

// A fixed pattern no two neighbours share, so filtering always blends.
static bool make_texture(void)
{
	static uint32_t texels[TEXTURE_SIZE * TEXTURE_SIZE];
	if (!NT_SUCCESS(xsdk_texture_create(&texture, XSDK_FORMAT_A8R8G8B8, TEXTURE_SIZE, TEXTURE_SIZE, 1, true)))
		return false;
	for (uint32_t y = 0; y < TEXTURE_SIZE; y++) {
		for (uint32_t x = 0; x < TEXTURE_SIZE; x++)
			texels[y * TEXTURE_SIZE + x] = 0x80000000u | (x * 4) << 16 | (y * 4) << 8 | ((x * 7 + y * 13) & 0xFF);
	}
	xsdk_texture_upload(&texture, 0, texels, TEXTURE_SIZE * 4);
	xsdk_gpu_invalidate(texture.address, texture.size_bytes);
	const XsdkSampler setting = { .mag_filter = XSDK_FILTER_LINEAR,
		.min_filter = XSDK_FILTER_LINEAR,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_WRAP,
		.address_v = XSDK_ADDRESS_WRAP };
	xsdk_texture_sampler_state(&sampler, &setting);
	return true;
}

static bool make_resources(void)
{
	const XsdkVertexElement position = { .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4 };
	image_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	image = xsdk_gpu_alloc(image_bytes);
	clocks = xsdk_gpu_alloc(2 * XSDK_GPU_CLOCK_WORDS * sizeof(uint32_t));
	visibility = xsdk_gpu_alloc(sizeof(uint32_t));
	quad_vertices = gpu_words(quad, QUAD_VERTICES);
	mesh_vertices = gpu_words(NULL, DRAW_VERTICES);
	if (!image || !clocks || !visibility || !quad_vertices || !mesh_vertices || !make_texture())
		return false;
	// Zeroed, so bytes past the 720 rows that the resolves may not write hash the same in every run.
	for (uint32_t i = 0; i < image_bytes / 4; i++)
		image[i] = 0;
	// Sixteenths from 1/16 to 1: none 0, varied, and fixed, so every run draws the same.
	for (uint32_t r = 0; r < XSDK_FLOAT_CONSTANTS; r++) {
		for (uint32_t c = 0; c < 4; c++)
			constants[r][c] = (float)(1 + (r * 7 + c * 3) % 16) / 16.0f;
	}
	return NT_SUCCESS(xsdk_shader_load(&probe_vertex, probe_vertex_shader, sizeof probe_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&probe_pixel, probe_pixel_shader, sizeof probe_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&probe_vertex, &position, 1, (const uint32_t[]){ VERTEX_BYTES }));
}

static bool is_exec(uint32_t opcode)
{
	return (opcode >= CF_EXEC_FIRST && opcode <= CF_EXEC_LAST) || opcode == CF_EXEC_PRED_CLEAN ||
		opcode == CF_EXEC_PRED_CLEAN_END;
}

// The ALU instructions the execs hold, walked as src/shader.c walks them: the control flow ends where the first exec's
// instructions begin (xenia:src/xenia/gpu/shader_translator.cc:60-72).
static uint32_t alu_count(const XsdkShader *shader)
{
	uint32_t pairs = shader->dword_count / SLOT_DWORDS;
	uint32_t count = 0;
	for (uint32_t pair = 0; pair < pairs; pair++) {
		const uint32_t *const d = &shader->source[pair * SLOT_DWORDS];
		const uint32_t words[2][2] = { { d[0], d[1] & 0xFFFF }, { d[1] >> 16 | d[2] << 16, d[2] >> 16 } };
		for (int i = 0; i < 2; i++) {
			if (!is_exec(words[i][1] >> CF_OPCODE_SHIFT & CF_OPCODE_MASK))
				continue;
			const uint32_t address = words[i][0] & EXEC_ADDRESS_MASK;
			const uint32_t instructions = words[i][0] >> EXEC_COUNT_SHIFT & EXEC_COUNT_MASK;
			const uint32_t sequence = words[i][0] >> EXEC_SEQUENCE_SHIFT;
			pairs = address < pairs ? address : pairs;
			for (uint32_t n = 0; n < instructions; n++)
				count += !(sequence >> (2 * n) & 1);
		}
	}
	return count;
}

static uint32_t register_count(const XsdkShader *shader)
{
	const uint32_t control = shader->program_control;
	return 1 +
		(shader->type == XSDK_SHADER_VERTEX ? control & VS_REGISTERS_MASK
											: control >> PS_REGISTERS_SHIFT & PS_REGISTERS_MASK);
}

// The fixed constants, then the shader's literals over them, then the probe's texture at each of its samplers' slots.
static void set_inputs(const XsdkXscShader *compiled, bool pixel)
{
	void (*const set)(uint32_t, const float *, uint32_t) =
		pixel ? xsdk_gpu_set_pixel_constants : xsdk_gpu_set_vertex_constants;
	set(0, constants[0], XSDK_FLOAT_CONSTANTS);
	for (size_t i = 0; i < compiled->need_count; i++) {
		const XsdkXscNeed *const n = &compiled->needs[i];
		if (n->kind != XSDK_XSC_LITERAL || n->reg >= XSDK_FLOAT_CONSTANTS)
			continue;
		const float values[4] = { (float)n->value[0], (float)n->value[1], (float)n->value[2], (float)n->value[3] };
		set(n->reg, values, 1);
	}
	for (size_t i = 0; i < compiled->resource_count; i++) {
		const XsdkXscResource *const r = &compiled->resources[i];
		if (r->kind != XSDK_XSC_SAMPLER)
			continue;
		for (uint32_t slot = r->reg; slot < r->reg + r->regs && slot < TEXTURE_SLOTS; slot++)
			xsdk_gpu_set_texture(slot, &texture, &sampler);
	}
}

// The GPU's clocks over DRAWS draws, the best of ROUNDS, its clock copied with the GPU idle before and after; false
// if the GPU didn't finish.
static bool time_draws(uint32_t vertex_count, uint64_t *best)
{
	const uint32_t address = xsdk_gpu_address(clocks);
	for (uint32_t round = 0; round < ROUNDS; round++) {
		xsdk_gpu_write_clock(address, true);
		for (uint32_t d = 0; d < DRAWS; d++)
			xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, vertex_count);
		xsdk_gpu_write_clock(address + XSDK_GPU_CLOCK_WORDS * sizeof(uint32_t), true);
		if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)))
			return false;
		const uint64_t taken = xsdk_gpu_clock_value(clocks + XSDK_GPU_CLOCK_WORDS) - xsdk_gpu_clock_value(clocks);
		if (round == 0 || taken < *best)
			*best = taken;
	}
	return true;
}

static double us_a_draw(uint64_t clocks_taken)
{
	return (double)clocks_taken * 1000.0 / GPU_CLOCKS_PER_MS / DRAWS;
}

// 32-bit FNV-1a (http://www.isthe.com/chongo/tech/comp/fnv/) over the resolved bytes, as tests/unroll_loops.c hashes.
static uint32_t hash_image(void)
{
	static uint8_t chunk[HASH_CHUNK_BYTES] __attribute__((aligned(128)));
	const uint64_t start = now_us();
	uint32_t hash = FNV_OFFSET;
	for (uint32_t at = 0; at < image_bytes; at += HASH_CHUNK_BYTES) {
		const uint32_t size = image_bytes - at < HASH_CHUNK_BYTES ? image_bytes - at : HASH_CHUNK_BYTES;
		memcpy(chunk, (const uint8_t *)image + at, size);
		for (uint32_t i = 0; i < size; i++)
			hash = (hash ^ chunk[i]) * FNV_PRIME;
	}
	hash_us += now_us() - start;
	return hash;
}

// Drawn over the target DRAWS times a round, then resolved, which clears it for the next, and hashed.
static bool time_pixel_shader(const ProbeProgram *p, const XsdkShader *shader, const XsdkXscShader *compiled, Totals *t)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_vertex_constants(0, interpolant_constants[0], INTERPOLANT_CONSTANTS);
	set_inputs(compiled, true);
	xsdk_gpu_set_shaders(&probe_vertex, shader);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(quad_vertices), QUAD_VERTICES * VERTEX_BYTES);
	uint64_t best = 0;
	if (!time_draws(QUAD_VERTICES, &best))
		return false;
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)))
		return false;
	const uint32_t alu = alu_count(shader);
	const uint32_t registers = register_count(shader);
	xsdk_log_write("shader cost probe: %s: pixel, %u ALU, %u GPRs, %.2f us a draw, image hash %08X", p->name,
		(unsigned)alu, (unsigned)registers, us_a_draw(best), (unsigned)hash_image());
	t->timed++;
	t->alu += alu;
	t->registers += registers;
	t->us += us_a_draw(best);
	return true;
}

// Each input the shader fetches a float4 from the start of every vertex, all of them alike.
static uint32_t mesh_layout(const XsdkShader *shader, XsdkVertexElement *elements)
{
	uint32_t count = 0;
	for (; count < shader->fetch_count && count < MAX_ELEMENTS; count++) {
		const uint32_t patch = shader->fetches[count];
		elements[count] = (XsdkVertexElement){ .usage = patch >> PATCH_USAGE_SHIFT & PATCH_FIELD_MASK,
			.usage_index = patch >> PATCH_INDEX_SHIFT & PATCH_FIELD_MASK,
			.format = XSDK_VERTEX_FLOAT4 };
	}
	return count;
}

// What the visibility query's word says of the draw it was around.
static const char *reached(uint32_t word)
{
	if (word == UNWRITTEN)
		return "the visibility query never landed";
	return word & 1u << VISIBILITY_ID % IDS_PER_WORD ? "SOME REACHED THE PIXELS" : "none reached the pixels";
}

// Drawn over the mesh, with a visibility query around one draw first to show nothing reached the pixels.
static bool time_vertex_shader(const ProbeProgram *p, XsdkShader *shader, const XsdkXscShader *compiled, Totals *t)
{
	XsdkVertexElement elements[MAX_ELEMENTS];
	const uint32_t element_count = mesh_layout(shader, elements);
	if (!NT_SUCCESS(
			xsdk_shader_set_vertex_layout(shader, elements, element_count, (const uint32_t[]){ VERTEX_BYTES }))) {
		xsdk_log_write("shader cost probe: %s: its %u vertex fetches couldn't be laid out; skipped", p->name,
			(unsigned)shader->fetch_count);
		return true;
	}
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	set_inputs(compiled, false);
	xsdk_gpu_set_shaders(shader, &probe_pixel);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(mesh_vertices), DRAW_VERTICES * VERTEX_BYTES);
	visibility[0] = UNWRITTEN;
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_begin_visibility(VISIBILITY_ID);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, DRAW_VERTICES);
	xsdk_gpu_end_visibility(VISIBILITY_ID, xsdk_gpu_address(visibility));
	uint64_t best = 0;
	if (!time_draws(DRAW_VERTICES, &best))
		return false;
	const uint32_t alu = alu_count(shader);
	const uint32_t registers = register_count(shader);
	xsdk_log_write("shader cost probe: %s: vertex, %u ALU, %u GPRs, %.2f us a draw of %u vertices, %s", p->name,
		(unsigned)alu, (unsigned)registers, us_a_draw(best), (unsigned)DRAW_VERTICES, reached(visibility[0]));
	t->timed++;
	t->alu += alu;
	t->registers += registers;
	t->us += us_a_draw(best);
	return true;
}

// Compiled as vulkan4xenon compiles, the optimiser on (vulkan4xenon:src/shader.c:336); false if the GPU didn't finish.
static bool time_program(const ProbeProgram *p, Totals *pixel, Totals *vertex)
{
	XsdkXscOptions options;
	xsdk_xsc_default_options(&options);
	options.optimize = true;
	XsdkXscShader compiled;
	const XsdkXscStatus status = xsdk_xsc_compile(p->spirv, p->spirv_words, &options, &compiled);
	if (status != XSDK_XSC_OK) {
		xsdk_log_write(
			"shader cost probe: %s: xost refused it, status %d: %s; skipped", p->name, status, compiled.message);
		xsdk_xsc_free(&compiled);
		return true;
	}
	XsdkShader shader;
	bool finished = true;
	if (NT_SUCCESS(xsdk_shader_load(&shader, (const uint32_t *)compiled.container, compiled.container_bytes))) {
		finished = shader.type == XSDK_SHADER_PIXEL ? time_pixel_shader(p, &shader, &compiled, pixel)
													: time_vertex_shader(p, &shader, &compiled, vertex);
		xsdk_shader_free(&shader);
	} else {
		xsdk_log_write("shader cost probe: %s: its container didn't load; skipped", p->name);
	}
	// The shader's fetch patch records point into the container (src/shader.c), so it goes last.
	xsdk_xsc_free(&compiled);
	return finished;
}

static void log_totals(const char *stage, const Totals *t, uint32_t count)
{
	xsdk_log_write(
		"shader cost probe: %s shaders: %u of %u timed, %u ALU, %.2f GPRs a shader, %.2f us for a draw of each", stage,
		(unsigned)t->timed, (unsigned)count, (unsigned)t->alu, t->timed ? (double)t->registers / t->timed : 0.0, t->us);
}

int main(void)
{
	const uint64_t start = now_us();
	xsdk_log_write("shader cost probe: %u programs, xost %s, best of %u rounds of %u draws", (unsigned)PROGRAM_COUNT,
		xsdk_xsc_version(), (unsigned)ROUNDS, (unsigned)DRAWS);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("shader cost probe: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	if (!make_resources()) {
		xsdk_log_write("shader cost probe: FAILED, the buffers, texture or shaders couldn't be made");
		return 1;
	}
	// Clears the target the first pixel shader draws over.
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	Totals pixel = { 0 };
	Totals vertex = { 0 };
	uint32_t pixel_count = 0;
	const char *hung = NULL;
	for (uint32_t i = 0; i < PROGRAM_COUNT && !hung; i++) {
		pixel_count += programs[i].pixel;
		if (!time_program(&programs[i], &pixel, &vertex))
			hung = programs[i].name;
	}
	log_totals("pixel", &pixel, pixel_count);
	log_totals("vertex", &vertex, PROGRAM_COUNT - pixel_count);
	xsdk_log_write("shader cost probe: hashing took %llu ms", (unsigned long long)(hash_us / 1000));
	if (hung) {
		xsdk_log_write("shader cost probe: FAILED, the GPU didn't finish %s's draws", hung);
		return 1;
	}
	status = xsdk_gpu_close();
	xsdk_log_write("shader cost probe: done in %llu ms, %u pixel and %u vertex shaders timed, xsdk_gpu_close 0x%08X",
		(unsigned long long)((now_us() - start) / 1000), (unsigned)pixel.timed, (unsigned)vertex.timed,
		(unsigned)status);
	return NT_SUCCESS(status) ? 0 : 1;
}
