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
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 120,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 4,
	CLEAR_ARGB = 0xFF202020,
	// Not a power of two: the linear pitch is wider than the texture, and the tiled layout spans 5 by 3 tiles.
	TEXTURE_WIDTH = 160,
	TEXTURE_HEIGHT = 96,
	LAYOUTS = 2,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	TEXCOORD_OFFSET_BYTES = 16,
	TEXTURE_SLOT = 0,
};

typedef struct {
	const char *name;
	bool tiled;
	// The quad's top left pixel; each texture is drawn one texel to a pixel.
	uint32_t quad_x;
	uint32_t quad_y;
	// The tiled texture swaps red and green and slants its blue bands the other way, so the two look different.
	bool transposed;
} Layout;

typedef struct {
	uint32_t x;
	uint32_t y;
} Texel;

// Side by side in every frame, so one capture shows both and a quad showing the other texture fails.
static const Layout layouts[LAYOUTS] = {
	{ .name = "linear", .tiled = false, .quad_x = 96, .quad_y = 192, .transposed = false },
	{ .name = "tiled", .tiled = true, .quad_x = 384, .quad_y = 192, .transposed = true },
};

static const XsdkVertexElement vertex_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.usage_index = 0,
		.format = XSDK_VERTEX_FLOAT2,
		.offset_bytes = TEXCOORD_OFFSET_BYTES },
};

// The corners and the middle of each texture, checked every frame.
static const Texel sampled[] = {
	{ 0, 0 },
	{ TEXTURE_WIDTH - 1, 0 },
	{ 0, TEXTURE_HEIGHT - 1 },
	{ TEXTURE_WIDTH - 1, TEXTURE_HEIGHT - 1 },
	{ TEXTURE_WIDTH / 2, TEXTURE_HEIGHT / 2 },
};

static uint32_t image[TEXTURE_HEIGHT][TEXTURE_WIDTH];

// Every texel of a texture differs, its red and green holding its x and y.
static uint32_t texel_argb(const Layout *layout, uint32_t x, uint32_t y)
{
	const uint32_t red = layout->transposed ? y : x;
	const uint32_t green = layout->transposed ? x : y;
	const uint32_t blue = 0x80 | ((layout->transposed ? x - y : x + y) & 0x7F);
	return 0xFF000000 | red << 16 | green << 8 | blue;
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

static bool inside(const Layout *layout, uint32_t x, uint32_t y)
{
	return x >= layout->quad_x && x < layout->quad_x + TEXTURE_WIDTH && y >= layout->quad_y &&
		y < layout->quad_y + TEXTURE_HEIGHT;
}

// What the pixel should hold: a texel inside either quad, the clear colour elsewhere.
static uint32_t expected_at(uint32_t x, uint32_t y)
{
	for (uint32_t i = 0; i < LAYOUTS; i++) {
		if (inside(&layouts[i], x, y))
			return as_stored(texel_argb(&layouts[i], x - layouts[i].quad_x, y - layouts[i].quad_y));
	}
	return as_stored(CLEAR_ARGB);
}

static void log_miss(const Layout *layout, uint32_t frame, uint32_t x, uint32_t y, uint32_t pixel)
{
	// A stored word's red and green bytes tell which texel, of which texture, it came from.
	xsdk_log_write("texture: %s: frame %u: (%u, %u) held %08X, red %u and green %u, not %08X", layout->name,
		(unsigned)frame, (unsigned)x, (unsigned)y, (unsigned)pixel, (unsigned)(pixel >> 8 & 0xFF),
		(unsigned)(pixel >> 16 & 0xFF), (unsigned)expected_at(x, y));
}

// The quad's sampled texels, and the pixel up and left of it, hold what they should.
static bool frame_holds(const Layout *layout, volatile const uint32_t *pixels, uint32_t frame, uint32_t *misses)
{
	bool holds = true;
	for (uint32_t i = 0; i <= sizeof sampled / sizeof sampled[0]; i++) {
		const bool outside = i == sizeof sampled / sizeof sampled[0];
		const uint32_t x = outside ? layout->quad_x - 1 : layout->quad_x + sampled[i].x;
		const uint32_t y = outside ? layout->quad_y - 1 : layout->quad_y + sampled[i].y;
		const uint32_t pixel = pixel_at(pixels, x, y);
		if (pixel == expected_at(x, y))
			continue;
		if (*misses < MAX_MISSES_LOGGED)
			log_miss(layout, frame, x, y, pixel);
		holds = false;
	}
	*misses += !holds;
	return holds;
}

// Every pixel of the quad and of a one-pixel border around it; returns how many are wrong.
static uint32_t count_wrong(const Layout *layout, volatile const uint32_t *pixels, uint32_t frame)
{
	uint32_t wrong = 0;
	for (uint32_t y = layout->quad_y - 1; y <= layout->quad_y + TEXTURE_HEIGHT; y++) {
		for (uint32_t x = layout->quad_x - 1; x <= layout->quad_x + TEXTURE_WIDTH; x++) {
			const uint32_t pixel = pixel_at(pixels, x, y);
			if (pixel == expected_at(x, y))
				continue;
			if (wrong < MAX_MISSES_LOGGED)
				log_miss(layout, frame, x, y, pixel);
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
static volatile uint32_t *upload_quad(const Layout *layout)
{
	const float left = clip_x(layout->quad_x - 0.5f);
	const float right = clip_x(layout->quad_x + TEXTURE_WIDTH - 0.5f);
	const float top = clip_y(layout->quad_y - 0.5f);
	const float bottom = clip_y(layout->quad_y + TEXTURE_HEIGHT - 0.5f);
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

static bool make_texture(XsdkTexture *texture, const Layout *layout)
{
	if (!NT_SUCCESS(
			xsdk_texture_create(texture, XSDK_FORMAT_A8R8G8B8, TEXTURE_WIDTH, TEXTURE_HEIGHT, 1, layout->tiled)))
		return false;
	for (uint32_t y = 0; y < TEXTURE_HEIGHT; y++)
		for (uint32_t x = 0; x < TEXTURE_WIDTH; x++)
			image[y][x] = texel_argb(layout, x, y);
	xsdk_texture_upload(texture, 0, &image[0][0], sizeof image[0]);
	return true;
}

typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *pixel;
	uint32_t vertices[LAYOUTS];
	const XsdkTexture *textures[LAYOUTS];
	// Nearest texel, edges clamped.
	const XsdkSamplerState *sampler;
	volatile uint32_t *const *buffers;
	bool has_display;
} Scene;

static void check_layout(const Layout *layout, const char *what, bool passed)
{
	char name[96];
	snprintf(name, sizeof name, "texture: %s: %s", layout->name, what);
	check(name, passed);
}

// Each frame draws both quads into eDRAM, resolves them into the buffer not on screen, clearing eDRAM, and shows it.
static void draw_frames(const Scene *scene)
{
	uint32_t misses[LAYOUTS] = { 0 };
	uint32_t wrong[LAYOUTS] = { 0 };
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = scene->buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_shaders(scene->vertex, scene->pixel);
		for (uint32_t i = 0; i < LAYOUTS; i++) {
			xsdk_gpu_set_vertices(scene->vertices[i], QUAD_VERTICES * STRIDE_BYTES);
			// Each quad rebinds slot 0, which the last frame's resolve also overwrote.
			xsdk_gpu_set_texture(TEXTURE_SLOT, scene->textures[i], scene->sampler);
			xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
		}
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		for (uint32_t i = 0; i < LAYOUTS && fences_landed; i++) {
			// The first frame's background is whatever eDRAM held; later ones are the clear colour.
			if (frame > 0)
				(void)frame_holds(&layouts[i], buffer, frame, &misses[i]);
			if (frame == FRAMES - 1)
				wrong[i] = count_wrong(&layouts[i], buffer, frame);
		}
		if (scene->has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	check("texture: every frame's fence landed", fences_landed);
	for (uint32_t i = 0; i < LAYOUTS; i++) {
		xsdk_log_write("texture: %s: the last frame had %u wrong pixels", layouts[i].name, (unsigned)wrong[i]);
		check_layout(&layouts[i], "every frame's corner and middle texels in place", fences_landed && misses[i] == 0);
		check_layout(&layouts[i], "the last frame holds every texel, one to a pixel", fences_landed && wrong[i] == 0);
	}
	if (scene->has_display)
		check("texture: every frame shown", shown);
}

void test_texture(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("texture: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("texture: display open at 1280x720", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, texture_vertex_shader, sizeof texture_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, texture_pixel_shader, sizeof texture_pixel_shader));
	check("texture: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, vertex_layout, sizeof vertex_layout / sizeof vertex_layout[0], STRIDE_BYTES));
	check("texture: vertex fetches patched for a float4 position and float2 coordinates", patched);
	XsdkTexture textures[LAYOUTS] = { { .texels = NULL }, { .texels = NULL } };
	volatile uint32_t *vertices[LAYOUTS] = { NULL, NULL };
	bool made = true;
	for (uint32_t i = 0; i < LAYOUTS; i++) {
		made = make_texture(&textures[i], &layouts[i]) && made;
		vertices[i] = upload_quad(&layouts[i]);
	}
	check("texture: linear and tiled textures made", made);
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("texture: vertex and front buffers", vertices[0] && vertices[1] && buffers[0] && buffers[1]);
	const XsdkSampler point_clamped = { .mag_filter = XSDK_FILTER_POINT,
		.min_filter = XSDK_FILTER_POINT,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP,
		.lod_bias = 0.0f };
	XsdkSamplerState sampler;
	xsdk_texture_sampler_state(&sampler, &point_clamped);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && made && vertices[0] && vertices[1] && buffers[0] && buffers[1]) {
		const Scene scene = { .vertex = &vertex,
			.pixel = &pixel,
			.vertices = { xsdk_gpu_address(vertices[0]), xsdk_gpu_address(vertices[1]) },
			.textures = { &textures[0], &textures[1] },
			.sampler = &sampler,
			.buffers = buffers,
			.has_display = has_display };
		draw_frames(&scene);
	}
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	for (uint32_t i = 0; i < LAYOUTS; i++) {
		xsdk_gpu_free(vertices[i]);
		xsdk_texture_free(&textures[i]);
	}
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("texture: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
