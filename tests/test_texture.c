#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tests.h"
#include "texture_shaders.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"
#include "xsdk/thread.h"

enum {
	WIDTH = 640,
	HEIGHT = 480,
	PIXELS = WIDTH * HEIGHT,
	BUFFERS = 2,
	FRAMES = 60,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 4,
	CLEAR_ARGB = 0xFF202020,
	// Not a power of two: the linear pitch is wider than the texture, and the tiled layout spans 5 by 3 tiles.
	TEXTURE_WIDTH = 160,
	TEXTURE_HEIGHT = 96,
	// The quad's top left pixel; the texture is drawn one texel to a pixel.
	QUAD_X = 64,
	QUAD_Y = 48,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	TEXCOORD_OFFSET_BYTES = 16,
	TEXTURE_SLOT = 0,
};

typedef struct {
	uint32_t x;
	uint32_t y;
} Texel;

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.usage_index = 0,
		.format = XSDK_VERTEX_FLOAT2,
		.offset_bytes = TEXCOORD_OFFSET_BYTES },
};

// The corners and the middle of the texture, checked every frame.
static const Texel sampled[] = {
	{ 0, 0 },
	{ TEXTURE_WIDTH - 1, 0 },
	{ 0, TEXTURE_HEIGHT - 1 },
	{ TEXTURE_WIDTH - 1, TEXTURE_HEIGHT - 1 },
	{ TEXTURE_WIDTH / 2, TEXTURE_HEIGHT / 2 },
};

static uint32_t image[TEXTURE_HEIGHT][TEXTURE_WIDTH];

// Every texel differs, red holding its x and green its y, so one out of place shows where it came from.
static uint32_t texel_argb(uint32_t x, uint32_t y)
{
	return 0xFF000000 | x << 16 | y << 8 | (0x80 | ((x + y) & 0x7F));
}

// A resolve stores the colour's bytes B, G, R, A, as the display reads them (docs/HARDWARE.md).
static uint32_t as_stored(uint32_t argb)
{
	return __builtin_bswap32(argb);
}

static uint32_t pixel_at(volatile const uint32_t *pixels, uint32_t x, uint32_t y)
{
	return pixels[tiled_index(x, y, WIDTH)];
}

// What the pixel should hold: its texel inside the quad, the clear colour around it.
static uint32_t expected_at(uint32_t x, uint32_t y)
{
	const bool inside = x >= QUAD_X && x < QUAD_X + TEXTURE_WIDTH && y >= QUAD_Y && y < QUAD_Y + TEXTURE_HEIGHT;
	return as_stored(inside ? texel_argb(x - QUAD_X, y - QUAD_Y) : CLEAR_ARGB);
}

static void log_miss(const char *name, uint32_t frame, uint32_t x, uint32_t y, uint32_t pixel)
{
	// A stored word's red and green bytes name the texel it came from.
	xsdk_log_write("texture: %s: frame %u: (%u, %u) held %08X, texel (%u, %u), not %08X", name, (unsigned)frame,
		(unsigned)x, (unsigned)y, (unsigned)pixel, (unsigned)(pixel >> 8 & 0xFF), (unsigned)(pixel >> 16 & 0xFF),
		(unsigned)expected_at(x, y));
}

// The sampled texels, and the pixel up and left of the quad, hold what they should.
static bool frame_holds(const char *name, volatile const uint32_t *pixels, uint32_t frame, uint32_t *misses)
{
	bool holds = true;
	for (uint32_t i = 0; i <= sizeof sampled / sizeof sampled[0]; i++) {
		const bool outside = i == sizeof sampled / sizeof sampled[0];
		const uint32_t x = outside ? QUAD_X - 1 : QUAD_X + sampled[i].x;
		const uint32_t y = outside ? QUAD_Y - 1 : QUAD_Y + sampled[i].y;
		const uint32_t pixel = pixel_at(pixels, x, y);
		if (pixel == expected_at(x, y))
			continue;
		if (*misses < MAX_MISSES_LOGGED)
			log_miss(name, frame, x, y, pixel);
		holds = false;
	}
	*misses += !holds;
	return holds;
}

// Every pixel of the quad and of a one-pixel border around it; returns how many are wrong.
static uint32_t count_wrong(const char *name, volatile const uint32_t *pixels, uint32_t frame)
{
	uint32_t wrong = 0;
	for (uint32_t y = QUAD_Y - 1; y <= QUAD_Y + TEXTURE_HEIGHT; y++) {
		for (uint32_t x = QUAD_X - 1; x <= QUAD_X + TEXTURE_WIDTH; x++) {
			const uint32_t pixel = pixel_at(pixels, x, y);
			if (pixel == expected_at(x, y))
				continue;
			if (wrong < MAX_MISSES_LOGGED)
				log_miss(name, frame, x, y, pixel);
			wrong++;
		}
	}
	return wrong;
}

// A screen position in clip space, through the viewport xsdk_gpu_set_target sets.
static float clip_x(float x)
{
	return x / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(float y)
{
	return 1.0f - y / (HEIGHT / 2.0f);
}

// Two triangles, edges half a pixel before the pixels, for Direct3D centres (xenia:src/xenia/gpu/draw_util.cc:837).
static volatile uint32_t *upload_quad(void)
{
	const float left = clip_x(QUAD_X - 0.5f);
	const float right = clip_x(QUAD_X + TEXTURE_WIDTH - 0.5f);
	const float top = clip_y(QUAD_Y - 0.5f);
	const float bottom = clip_y(QUAD_Y + TEXTURE_HEIGHT - 0.5f);
	const float quad[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, 0.0f, 1.0f, 0.0f, 0.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
	};
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof quad);
	if (!vertices)
		return NULL;
	uint32_t words[sizeof quad / 4];
	memcpy(words, quad, sizeof quad);
	for (uint32_t i = 0; i < sizeof quad / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	return vertices;
}

static bool make_texture(XsdkTexture *texture, bool tiled)
{
	if (!NT_SUCCESS(xsdk_texture_create(texture, TEXTURE_WIDTH, TEXTURE_HEIGHT, tiled)))
		return false;
	xsdk_texture_upload(texture, &image[0][0], TEXTURE_WIDTH);
	return true;
}

typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *pixel;
	uint32_t vertices;
	volatile uint32_t *const *buffers;
	bool has_display;
} Scene;

// Each frame draws the quad into eDRAM, resolves it into the buffer not on screen, clearing eDRAM, and shows it.
static void draw_frames(const char *name, const Scene *scene, const XsdkTexture *texture)
{
	uint32_t misses = 0;
	uint32_t wrong = 0;
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = scene->buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_shaders(scene->vertex, scene->pixel);
		xsdk_gpu_set_vertices(scene->vertices, QUAD_VERTICES * STRIDE_BYTES);
		// The last frame's resolve overwrote slot 0.
		xsdk_gpu_set_texture(TEXTURE_SLOT, texture);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		// The first frame's background is whatever eDRAM held; later ones are the clear colour.
		if (frame > 0 && fences_landed)
			(void)frame_holds(name, buffer, frame, &misses);
		if (frame == FRAMES - 1 && fences_landed)
			wrong = count_wrong(name, buffer, frame);
		if (scene->has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write("texture: %s: the last frame had %u wrong pixels", name, (unsigned)wrong);
	char what[96];
	snprintf(what, sizeof what, "texture: %s: every frame's fence landed", name);
	check(what, fences_landed);
	snprintf(what, sizeof what, "texture: %s: every frame's corner and middle texels in place", name);
	check(what, fences_landed && misses == 0);
	snprintf(what, sizeof what, "texture: %s: the last frame holds every texel, one to a pixel", name);
	check(what, fences_landed && wrong == 0);
	if (scene->has_display) {
		snprintf(what, sizeof what, "texture: %s: every frame shown", name);
		check(what, shown);
	}
}

void test_texture(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("texture: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display = NT_SUCCESS(xsdk_display_open(&width, &height)) && width == WIDTH && height == HEIGHT;
	check("texture: display open in a 640x480 mode", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, texture_vertex_shader, sizeof texture_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, texture_pixel_shader, sizeof texture_pixel_shader));
	check("texture: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex, layout, sizeof layout / sizeof layout[0], STRIDE_BYTES));
	check("texture: vertex fetches patched for a float4 position and float2 coordinates", patched);
	for (uint32_t y = 0; y < TEXTURE_HEIGHT; y++)
		for (uint32_t x = 0; x < TEXTURE_WIDTH; x++)
			image[y][x] = texel_argb(x, y);
	XsdkTexture linear = { .texels = NULL };
	XsdkTexture tiled = { .texels = NULL };
	const bool made = make_texture(&linear, false) && make_texture(&tiled, true);
	check("texture: linear and tiled textures made", made);
	volatile uint32_t *const vertices = upload_quad();
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(PIXELS * 4), xsdk_gpu_alloc(PIXELS * 4) };
	check("texture: vertex and front buffers", vertices && buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && made && vertices && buffers[0] && buffers[1]) {
		const Scene scene = { .vertex = &vertex,
			.pixel = &pixel,
			.vertices = xsdk_gpu_address(vertices),
			.buffers = buffers,
			.has_display = has_display };
		draw_frames("linear", &scene, &linear);
		draw_frames("tiled", &scene, &tiled);
	}
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	xsdk_texture_free(&tiled);
	xsdk_texture_free(&linear);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("texture: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
