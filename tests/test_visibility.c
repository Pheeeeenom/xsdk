#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "constant_shaders.h"
#include "tests.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// Visibility queries (vulkan4xenon milestone 28), each status word copied to memory and its bit checked there: a quad
// sets its query's bit, no draw leaves it clear, a quad hidden behind another sets it too, an id begun again around no
// draw after a quad comes back clear, which the console hasn't shown yet, and ids 32 to 63 answer in the second word.
// Xenia sets the bit at every query's end (xenia:src/xenia/gpu/command_processor.cc:1622-1630), so the clear ones fail
// there.

enum {
	WIDTH = 128,
	HEIGHT = 64,
	FENCE_TIMEOUT_MS = 1000,
	VERTEX_FLOATS = 4,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CLEAR_ARGB = 0xFF202020,
	QUAD_ARGB = 0xFF40E040,
	HIDDEN_ARGB = 0xFFE04040,
	// What the words hold till a copy lands.
	UNWRITTEN = 0xA5A5A5A5,
	IDS_PER_WORD = 32,
	HIGH_ID_DRAWN = 63,
	HIGH_ID_EMPTY = 32,
};

// The queries in the order they run, a word each.
enum {
	QUERY_QUAD,
	QUERY_NONE,
	QUERY_HIDDEN,
	QUERY_REUSED,
	QUERY_HIGH_DRAWN,
	QUERY_HIGH_EMPTY,
	QUERIES,
};

static const uint32_t ids[QUERIES] = { 1, 2, 3, 1, HIGH_ID_DRAWN, HIGH_ID_EMPTY };

// The quad is drawn first at 0.5, the hidden quad behind it at 0.75, and the high id's quad in front of both, each
// over the whole target, depth less-than and written.
static const float quad_depth = 0.5f;
static const float hidden_depth = 0.75f;
static const float front_depth = 0.25f;

static const float square[QUAD_VERTICES][VERTEX_FLOATS] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
};

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

// The constant vertex shader places the unit square over the whole target at the depth in c1.z, coloured by c2.
static void draw_quad(float depth, uint32_t argb)
{
	const float vertex[3][4] = {
		{ 2.0f, -2.0f, 1.0f, 1.0f },
		{ -1.0f - 1.0f / WIDTH, 1.0f + 1.0f / HEIGHT, depth, 0.0f },
		{ unorm(argb >> 16), unorm(argb >> 8), unorm(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *pixel;
	uint32_t vertices;
	uint32_t target;
	volatile uint32_t *words;
} Scene;

// Colour and depth cleared to far by a resolve, as tests/probe_occlusion.c does, then the target set again.
static void clear(const Scene *scene)
{
	xsdk_gpu_set_depth_clear(1.0f, 0);
	xsdk_gpu_resolve(scene->target, WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(scene->vertex, scene->pixel);
	xsdk_gpu_set_vertices(0, scene->vertices, sizeof square);
	XsdkDepthStencilState less;
	const XsdkDepthStencil setting = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS };
	xsdk_gpu_depth_stencil_state(&less, &setting);
	xsdk_gpu_set_depth_stencil(&less);
}

static void query(const Scene *scene, uint32_t index, bool drawn, float depth, uint32_t argb)
{
	xsdk_gpu_begin_visibility(ids[index]);
	if (drawn)
		draw_quad(depth, argb);
	xsdk_gpu_end_visibility(ids[index], xsdk_gpu_address(scene->words) + index * 4);
}

// The query's bit in the word copied at its end, which must have landed.
static bool answered(const Scene *scene, uint32_t index, bool set)
{
	const uint32_t word = scene->words[index];
	return word != UNWRITTEN && (word >> ids[index] % IDS_PER_WORD & 1) == set;
}

static void check_queries(const Scene *scene)
{
	for (uint32_t i = 0; i < QUERIES; i++)
		scene->words[i] = UNWRITTEN;
	__asm__ volatile("sync" ::: "memory");
	clear(scene);
	query(scene, QUERY_QUAD, true, quad_depth, QUAD_ARGB);
	query(scene, QUERY_NONE, false, 0.0f, 0);
	query(scene, QUERY_HIDDEN, true, hidden_depth, HIDDEN_ARGB);
	query(scene, QUERY_REUSED, false, 0.0f, 0);
	query(scene, QUERY_HIGH_DRAWN, true, front_depth, QUAD_ARGB);
	query(scene, QUERY_HIGH_EMPTY, false, 0.0f, 0);
	const bool landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	check("visibility: the queries' fence landed", landed);
	for (uint32_t i = 0; i < QUERIES; i++)
		xsdk_log_write(
			"visibility: query %u, id %u: status word %08X", (unsigned)i, (unsigned)ids[i], (unsigned)scene->words[i]);
	check("visibility: a query around a drawn quad sets its bit", landed && answered(scene, QUERY_QUAD, true));
	check("visibility: a query around no draw leaves its bit clear", landed && answered(scene, QUERY_NONE, false));
	const bool hidden_set = landed && answered(scene, QUERY_HIDDEN, true);
	xsdk_log_write("visibility: the hidden quad's query came back %s; set is the conservative answer the probe found",
		hidden_set ? "set" : "clear");
	check("visibility: a query around a quad hidden behind another sets its bit too, the conservative answer",
		hidden_set);
	check("visibility: an id begun again around no draw, after a quad set its bit, comes back clear",
		landed && answered(scene, QUERY_REUSED, false));
	check("visibility: ids 32 to 63 answer in the second status word, 63's quad set and 32's no draw clear",
		landed && answered(scene, QUERY_HIGH_DRAWN, true) && answered(scene, QUERY_HIGH_EMPTY, false));
}

static volatile uint32_t *upload(const void *data, uint32_t bytes)
{
	volatile uint32_t *const vertices = xsdk_gpu_alloc(bytes);
	if (!vertices)
		return NULL;
	for (uint32_t i = 0; i < bytes / 4; i++) {
		uint32_t word;
		memcpy(&word, (const uint8_t *)data + i * 4, sizeof word);
		vertices[i] = word;
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), bytes);
	return vertices;
}

void test_visibility(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("visibility: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	volatile uint32_t *const vertices = upload(square, sizeof square);
	volatile uint32_t *const target = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	volatile uint32_t *const words = xsdk_gpu_alloc(QUERIES * 4);
	const bool made = loaded && vertices && target && words;
	check("visibility: shaders, vertices, the target's memory and the words made", made);
	if (made) {
		const Scene scene = { .vertex = &vertex,
			.pixel = &pixel,
			.vertices = xsdk_gpu_address(vertices),
			.target = xsdk_gpu_address(target),
			.words = words };
		check_queries(&scene);
	}
	xsdk_gpu_free(words);
	xsdk_gpu_free(target);
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("visibility: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
