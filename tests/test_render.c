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
	// The first half renders the source mirrored left to right, the second top to bottom.
	HALF = FRAMES / 2,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 4,
	CLEAR_ARGB = 0xFF202020,
	// The render target starts magenta, so a resolve that never writes it shows.
	SENTINEL_ARGB = 0xFFFF00FF,
	// A resolve's width is a multiple of 32; the tiled layout spans 5 by 3 tiles.
	TEXTURE_WIDTH = 160,
	TEXTURE_HEIGHT = 96,
	QUAD_Y = 192,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	TEXCOORD_OFFSET_BYTES = 16,
	TEXTURE_SLOT = 0,
	SIDES = 2,
	MIRRORS = 2,
};

typedef struct {
	const char *name;
	// The quad's top left pixel on screen; it shows its texture one texel to a pixel.
	uint32_t quad_x;
	// The source as uploaded, or the render target, which holds it mirrored.
	bool rendered;
} Side;

typedef struct {
	uint32_t x;
	uint32_t y;
} Texel;

// Side by side in every frame: the original on the left, the second pass's copy of the first on the right.
static const Side sides[SIDES] = {
	{ .name = "source", .quad_x = 96, .rendered = false },
	{ .name = "rendered", .quad_x = 384, .rendered = true },
};

static const XsdkVertexElement vertex_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.usage_index = 0,
		.format = XSDK_VERTEX_FLOAT2,
		.offset_bytes = TEXCOORD_OFFSET_BYTES },
};

// The corners and the middle of each quad, checked every frame.
static const Texel sampled[] = {
	{ 0, 0 },
	{ TEXTURE_WIDTH - 1, 0 },
	{ 0, TEXTURE_HEIGHT - 1 },
	{ TEXTURE_WIDTH - 1, TEXTURE_HEIGHT - 1 },
	{ TEXTURE_WIDTH / 2, TEXTURE_HEIGHT / 2 },
};

static uint32_t image[TEXTURE_HEIGHT][TEXTURE_WIDTH];

// Every texel of the source differs, its red and green holding its x and y.
static uint32_t source_argb(uint32_t x, uint32_t y)
{
	return 0xFF000000 | x << 16 | y << 8 | (0x80 | ((x + y) & 0x7F));
}

// The texel a quad shows at (x, y) in this frame: the source's own, or mirrored as the first pass drew it.
static uint32_t texel_argb(const Side *side, uint32_t frame, uint32_t x, uint32_t y)
{
	if (!side->rendered)
		return source_argb(x, y);
	return frame < HALF ? source_argb(TEXTURE_WIDTH - 1 - x, y) : source_argb(x, TEXTURE_HEIGHT - 1 - y);
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

static bool inside(const Side *side, uint32_t x, uint32_t y)
{
	return x >= side->quad_x && x < side->quad_x + TEXTURE_WIDTH && y >= QUAD_Y && y < QUAD_Y + TEXTURE_HEIGHT;
}

// What a screen pixel should hold in this frame: a texel inside either quad, the clear colour elsewhere.
static uint32_t expected_at(uint32_t frame, uint32_t x, uint32_t y)
{
	for (uint32_t i = 0; i < SIDES; i++) {
		if (inside(&sides[i], x, y))
			return as_stored(texel_argb(&sides[i], frame, x - sides[i].quad_x, y - QUAD_Y));
	}
	return as_stored(CLEAR_ARGB);
}

static void log_miss(const Side *side, uint32_t frame, uint32_t x, uint32_t y, uint32_t pixel)
{
	// A stored word's red and green bytes name the source texel it came from.
	xsdk_log_write("render: %s: frame %u: (%u, %u) held %08X, red %u and green %u, not %08X", side->name,
		(unsigned)frame, (unsigned)x, (unsigned)y, (unsigned)pixel, (unsigned)(pixel >> 8 & 0xFF),
		(unsigned)(pixel >> 16 & 0xFF), (unsigned)expected_at(frame, x, y));
}

// The quad's sampled texels, and the pixel up and left of it, hold what they should.
static bool frame_holds(const Side *side, volatile const uint32_t *pixels, uint32_t frame, uint32_t *misses)
{
	bool holds = true;
	for (uint32_t i = 0; i <= sizeof sampled / sizeof sampled[0]; i++) {
		const bool outside = i == sizeof sampled / sizeof sampled[0];
		const uint32_t x = outside ? side->quad_x - 1 : side->quad_x + sampled[i].x;
		const uint32_t y = outside ? QUAD_Y - 1 : QUAD_Y + sampled[i].y;
		const uint32_t pixel = pixel_at(pixels, x, y);
		if (pixel == expected_at(frame, x, y))
			continue;
		if (*misses < MAX_MISSES_LOGGED)
			log_miss(side, frame, x, y, pixel);
		holds = false;
	}
	*misses += !holds;
	return holds;
}

// Every pixel of the quad and of a one-pixel border around it; returns how many are wrong.
static uint32_t count_wrong(const Side *side, volatile const uint32_t *pixels, uint32_t frame)
{
	uint32_t wrong = 0;
	for (uint32_t y = QUAD_Y - 1; y <= QUAD_Y + TEXTURE_HEIGHT; y++) {
		for (uint32_t x = side->quad_x - 1; x <= side->quad_x + TEXTURE_WIDTH; x++) {
			const uint32_t pixel = pixel_at(pixels, x, y);
			if (pixel == expected_at(frame, x, y))
				continue;
			if (wrong < MAX_MISSES_LOGGED)
				log_miss(side, frame, x, y, pixel);
			wrong++;
		}
	}
	return wrong;
}

// A quad at (x, y), its texture flipped as asked, edges half a pixel early (xenia:src/xenia/gpu/draw_util.cc:837).
static volatile uint32_t *upload_quad(
	uint32_t target_width, uint32_t target_height, uint32_t x, uint32_t y, bool flip_across, bool flip_down)
{
	const float left = (x - 0.5f) / (target_width / 2.0f) - 1.0f;
	const float right = (x + TEXTURE_WIDTH - 0.5f) / (target_width / 2.0f) - 1.0f;
	const float top = 1.0f - (y - 0.5f) / (target_height / 2.0f);
	const float bottom = 1.0f - (y + TEXTURE_HEIGHT - 0.5f) / (target_height / 2.0f);
	const float u0 = flip_across ? 1.0f : 0.0f;
	const float u1 = 1.0f - u0;
	const float v0 = flip_down ? 1.0f : 0.0f;
	const float v1 = 1.0f - v0;
	const float quad[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, 0.0f, 1.0f, u0, v0 },
		{ right, top, 0.0f, 1.0f, u1, v0 },
		{ left, bottom, 0.0f, 1.0f, u0, v1 },
		{ left, bottom, 0.0f, 1.0f, u0, v1 },
		{ right, top, 0.0f, 1.0f, u1, v0 },
		{ right, bottom, 0.0f, 1.0f, u1, v1 },
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

static bool make_texture(XsdkTexture *texture, bool rendered)
{
	if (!NT_SUCCESS(xsdk_texture_create(texture, XSDK_FORMAT_A8R8G8B8, TEXTURE_WIDTH, TEXTURE_HEIGHT, 1, true)))
		return false;
	for (uint32_t y = 0; y < TEXTURE_HEIGHT; y++)
		for (uint32_t x = 0; x < TEXTURE_WIDTH; x++)
			image[y][x] = rendered ? SENTINEL_ARGB : source_argb(x, y);
	xsdk_texture_upload(texture, 0, &image[0][0], sizeof image[0]);
	return true;
}

typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *pixel;
	// The first pass's quads filling the render target, mirrored left to right, then top to bottom.
	uint32_t mirrored[MIRRORS];
	// The second pass's quads on screen, one a side.
	uint32_t shown[SIDES];
	const XsdkTexture *textures[SIDES];
	// Nearest texel, edges clamped.
	const XsdkSamplerState *sampler;
	volatile uint32_t *const *buffers;
	bool has_display;
} Scene;

static void check_side(const Side *side, const char *what, bool passed)
{
	char name[112];
	snprintf(name, sizeof name, "render: %s: %s", side->name, what);
	check(name, passed);
}

static void draw_frame(const Scene *scene, uint32_t frame, uint32_t front_buffer)
{
	// The first pass draws the source, mirrored, over the whole render target, then resolves it into the texture.
	xsdk_gpu_set_target(TEXTURE_WIDTH, TEXTURE_HEIGHT);
	xsdk_gpu_set_shaders(scene->vertex, scene->pixel);
	xsdk_gpu_set_vertices(scene->mirrored[frame < HALF ? 0 : 1], QUAD_BYTES);
	xsdk_gpu_set_texture(TEXTURE_SLOT, scene->textures[0], scene->sampler);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	xsdk_gpu_resolve_texture(scene->textures[1], CLEAR_ARGB);
	// The second pass draws the source and the render target side by side; the resolve undid the target and shader.
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(scene->vertex, scene->pixel);
	for (uint32_t i = 0; i < SIDES; i++) {
		xsdk_gpu_set_vertices(scene->shown[i], QUAD_BYTES);
		xsdk_gpu_set_texture(TEXTURE_SLOT, scene->textures[i], scene->sampler);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
	xsdk_gpu_resolve(front_buffer, WIDTH, HEIGHT, CLEAR_ARGB);
}

// Each frame renders the texture, draws both quads, resolves them into the buffer not on screen, and shows it.
static void draw_frames(const Scene *scene)
{
	uint32_t misses[SIDES] = { 0 };
	uint32_t wrong[SIDES] = { 0 };
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = scene->buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		draw_frame(scene, frame, address);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		for (uint32_t i = 0; i < SIDES && fences_landed; i++) {
			// The first frame's background is whatever eDRAM held; later ones are the clear colour.
			if (frame > 0)
				(void)frame_holds(&sides[i], buffer, frame, &misses[i]);
			if (frame == HALF - 1 || frame == FRAMES - 1)
				wrong[i] += count_wrong(&sides[i], buffer, frame);
		}
		if (scene->has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	check("render: every frame's fence landed", fences_landed);
	for (uint32_t i = 0; i < SIDES; i++) {
		xsdk_log_write(
			"render: %s: the last frame of each half had %u wrong pixels", sides[i].name, (unsigned)wrong[i]);
		check_side(&sides[i], "every frame's corner and middle texels in place", fences_landed && misses[i] == 0);
		check_side(&sides[i], "the last frame of each half holds every texel", fences_landed && wrong[i] == 0);
	}
	if (scene->has_display)
		check("render: every frame shown", shown);
}

void test_render(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("render: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("render: display open at 1280x720", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, texture_vertex_shader, sizeof texture_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, texture_pixel_shader, sizeof texture_pixel_shader));
	check("render: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, vertex_layout, sizeof vertex_layout / sizeof vertex_layout[0], STRIDE_BYTES));
	check("render: vertex fetches patched", patched);
	XsdkTexture textures[SIDES] = { { .texels = NULL }, { .texels = NULL } };
	bool made = true;
	for (uint32_t i = 0; i < SIDES; i++)
		made = make_texture(&textures[i], sides[i].rendered) && made;
	check("render: source texture and render target made", made);
	volatile uint32_t *const vertices[MIRRORS + SIDES] = {
		upload_quad(TEXTURE_WIDTH, TEXTURE_HEIGHT, 0, 0, true, false),
		upload_quad(TEXTURE_WIDTH, TEXTURE_HEIGHT, 0, 0, false, true),
		upload_quad(WIDTH, HEIGHT, sides[0].quad_x, QUAD_Y, false, false),
		upload_quad(WIDTH, HEIGHT, sides[1].quad_x, QUAD_Y, false, false),
	};
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	bool allocated = buffers[0] && buffers[1];
	for (uint32_t i = 0; i < MIRRORS + SIDES; i++)
		allocated = allocated && vertices[i];
	check("render: vertex and front buffers", allocated);
	const XsdkSampler point_clamped = { .mag_filter = XSDK_FILTER_POINT,
		.min_filter = XSDK_FILTER_POINT,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP,
		.lod_bias = 0.0f };
	XsdkSamplerState sampler;
	xsdk_texture_sampler_state(&sampler, &point_clamped);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && made && allocated) {
		const Scene scene = { .vertex = &vertex,
			.pixel = &pixel,
			.mirrored = { xsdk_gpu_address(vertices[0]), xsdk_gpu_address(vertices[1]) },
			.shown = { xsdk_gpu_address(vertices[2]), xsdk_gpu_address(vertices[3]) },
			.textures = { &textures[0], &textures[1] },
			.sampler = &sampler,
			.buffers = buffers,
			.has_display = has_display };
		draw_frames(&scene);
	}
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	for (uint32_t i = 0; i < MIRRORS + SIDES; i++)
		xsdk_gpu_free(vertices[i]);
	for (uint32_t i = 0; i < SIDES; i++)
		xsdk_texture_free(&textures[i]);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("render: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
