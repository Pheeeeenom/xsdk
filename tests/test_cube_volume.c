#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tests.h"
#include "tiled_index.h"
#include "volume_shaders.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"
#include "xsdk/thread.h"

// Cube and 3D textures: every face, slice and level its own colours, each drawn one texel to a pixel where Vulkan's
// directions and coordinates point. The shaders are xost's (tools/gen_volume_shaders.py).

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 30,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 8,
	CLEAR_ARGB = 0xFF202020,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	COORD_OFFSET_BYTES = 16,
	TEXTURE_SLOT = 0,
	MAX_CELLS = 48,
	CELL_GAP = 8,
	FIRST_X = 40,
	MAX_SIDE = 32,
	HUES = 9,
	// Each cell's centre and corners, checked every frame.
	POINTS_PER_CELL = 5,
};

enum {
	TEXTURE_CUBE,
	TEXTURE_SMALL_CUBE,
	TEXTURE_VOLUME,
	TEXTURE_SMALL_VOLUME,
	TEXTURES,
};

typedef struct {
	const char *name;
	uint32_t dimension;
	uint32_t size;
	// Faces or slices.
	uint32_t depth;
	uint32_t levels;
} TextureSpec;

// A cube whose tail starts at level 1, and one whose base is in each face's tail; a 3D texture of two tiles' slices,
// and one of a part tile whose base is in its tail.
static const TextureSpec specs[TEXTURES] = {
	[TEXTURE_CUBE] = { "32x32 cube", XSDK_TEXTURE_CUBE, 32, XSDK_CUBE_FACES, 6 },
	[TEXTURE_SMALL_CUBE] = { "8x8 cube", XSDK_TEXTURE_CUBE, 8, XSDK_CUBE_FACES, 4 },
	[TEXTURE_VOLUME] = { "32x32x8 3D texture", XSDK_TEXTURE_3D, 32, 8, 1 },
	[TEXTURE_SMALL_VOLUME] = { "8x8x3 3D texture", XSDK_TEXTURE_3D, 8, 3, 1 },
};

enum {
	GROUP_CUBE,
	GROUP_SMALL_CUBE,
	GROUP_VOLUME,
	GROUP_SMALL_VOLUME,
	GROUPS,
};

static const char *const group_checks[GROUPS] = {
	"cube and 3D: a 32x32 cube's six faces at levels 0 and 1, each where Vulkan's directions point",
	"cube and 3D: an 8x8 cube's six faces at levels 0 and 2, from each face's packed tail",
	"cube and 3D: a 32x32x8 texture's eight slices, each at its w",
	"cube and 3D: an 8x8x3 texture's three slices, from its packed tail",
};

typedef struct {
	uint32_t group;
	uint32_t texture;
	// The face or slice shown.
	uint32_t slice;
	uint32_t level;
	uint32_t x;
	uint32_t y;
	uint32_t size;
	// A 3D cell's w, the same over it.
	float w;
	XsdkSampler sampler;
} Cell;

typedef struct {
	uint32_t cell;
	uint32_t x;
	uint32_t y;
} Point;

static const XsdkVertexElement vertex_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.usage_index = 0,
		.format = XSDK_VERTEX_FLOAT4,
		.offset_bytes = COORD_OFFSET_BYTES },
};

// Low five bits clear, so a texel's place fits under them.
static const uint32_t hues[HUES] = { 0xE02020, 0x20C020, 0x2040E0, 0xE0E020, 0xE020E0, 0x20E0E0, 0xE0E0E0, 0xA06020,
	0x6020A0 };

static uint32_t image[MAX_SIDE * MAX_SIDE];

static uint32_t level_size(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

// A hue for each face or slice and level; x and y in red and green's low bits, the face or slice and level in blue's.
static uint32_t texel_argb(uint32_t texture, uint32_t slice, uint32_t level, uint32_t x, uint32_t y)
{
	const uint32_t hue = hues[(texture * 3 + slice * 2 + level) % HUES];
	return 0xFF000000 | hue | (x & 31) << 16 | (y & 31) << 8 | slice << 2 | level;
}

static void fill_image(uint32_t texture, uint32_t slice, uint32_t level, uint32_t side)
{
	for (uint32_t y = 0; y < side; y++)
		for (uint32_t x = 0; x < side; x++)
			image[y * side + x] = texel_argb(texture, slice, level, x, y);
}

static bool make_texture(XsdkTexture *texture, uint32_t which)
{
	const TextureSpec *const spec = &specs[which];
	const NTSTATUS status = spec->dimension == XSDK_TEXTURE_CUBE
		? xsdk_texture_init_cube(texture, XSDK_FORMAT_A8R8G8B8, spec->size, spec->levels)
		: xsdk_texture_init_3d(texture, XSDK_FORMAT_A8R8G8B8, spec->size, spec->size, spec->depth);
	volatile uint32_t *const texels = NT_SUCCESS(status) ? xsdk_gpu_alloc(texture->size_bytes) : NULL;
	if (!texels) {
		xsdk_log_write("cube and 3D: %s not made: 0x%08X", spec->name, (unsigned)status);
		return false;
	}
	xsdk_texture_attach(texture, texels);
	for (uint32_t level = 0; level < spec->levels; level++) {
		const uint32_t side = level_size(spec->size, level);
		for (uint32_t slice = 0; slice < spec->depth; slice++) {
			fill_image(which, slice, level, side);
			xsdk_texture_upload_slice_rows(texture, level, slice, 0, side, image, side * sizeof image[0]);
		}
	}
	xsdk_gpu_invalidate(texture->address, texture->size_bytes);
	return true;
}

// Every face or slice of every level, read back.
static bool reads_back(const XsdkTexture *texture, uint32_t which)
{
	static uint32_t back[MAX_SIDE * MAX_SIDE];
	const TextureSpec *const spec = &specs[which];
	bool same = true;
	for (uint32_t level = 0; level < spec->levels; level++) {
		const uint32_t side = level_size(spec->size, level);
		for (uint32_t slice = 0; slice < spec->depth; slice++) {
			fill_image(which, slice, level, side);
			memset(back, 0, sizeof back);
			xsdk_texture_download_slice_rows(texture, level, slice, 0, side, back, side * sizeof back[0]);
			same = same && memcmp(back, image, side * side * sizeof image[0]) == 0;
		}
	}
	return same;
}

static const XsdkSampler point_clamped = { .mag_filter = XSDK_FILTER_POINT,
	.min_filter = XSDK_FILTER_POINT,
	.mip_filter = XSDK_FILTER_POINT,
	.address_u = XSDK_ADDRESS_CLAMP,
	.address_v = XSDK_ADDRESS_CLAMP };

// A row of cells, one a face or slice from the first, left to right; returns the count.
static uint32_t add_row(
	Cell *cells, uint32_t count, uint32_t group, uint32_t texture, uint32_t level, uint32_t slices, uint32_t y)
{
	const TextureSpec *const spec = &specs[texture];
	const uint32_t size = level_size(spec->size, level);
	for (uint32_t slice = 0; slice < slices; slice++) {
		Cell cell = { group, texture, slice, level, FIRST_X + slice * (size + CELL_GAP), y, size,
			((float)slice + 0.5f) / (float)spec->depth, point_clamped };
		// The level alone, whatever the GPU works out from the cube's coordinates.
		cell.sampler.min_level = level;
		cell.sampler.level_count = level + 1;
		cells[count++] = cell;
	}
	return count;
}

static uint32_t make_cells(Cell *cells)
{
	uint32_t count = add_row(cells, 0, GROUP_CUBE, TEXTURE_CUBE, 0, XSDK_CUBE_FACES, 40);
	count = add_row(cells, count, GROUP_CUBE, TEXTURE_CUBE, 1, XSDK_CUBE_FACES, 80);
	count = add_row(cells, count, GROUP_SMALL_CUBE, TEXTURE_SMALL_CUBE, 0, XSDK_CUBE_FACES, 104);
	count = add_row(cells, count, GROUP_SMALL_CUBE, TEXTURE_SMALL_CUBE, 2, XSDK_CUBE_FACES, 120);
	count = add_row(cells, count, GROUP_VOLUME, TEXTURE_VOLUME, 0, specs[TEXTURE_VOLUME].depth, 130);
	return add_row(cells, count, GROUP_SMALL_VOLUME, TEXTURE_SMALL_VOLUME, 0, specs[TEXTURE_SMALL_VOLUME].depth, 170);
}

// What the cell's pixel i across and j down should hold: one texel to a pixel, at its centre.
static uint32_t expected_argb(const Cell *cell, uint32_t i, uint32_t j)
{
	return texel_argb(cell->texture, cell->slice, cell->level, i, j);
}

// A resolve stores the colour's bytes B, G, R, A; alpha isn't checked.
static bool holds(uint32_t stored, uint32_t argb, uint32_t tolerance)
{
	const uint32_t want = __builtin_bswap32(argb);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > tolerance)
			return false;
	}
	return true;
}

static void log_miss(const Cell *cell, uint32_t frame, uint32_t x, uint32_t y, uint32_t stored, uint32_t argb)
{
	// A stored word's low blue bits name the face or slice and level it came from.
	xsdk_log_write("cube and 3D: frame %u: %s, slice %u, level %u, at (%u, %u) held %08X, not %08X", (unsigned)frame,
		specs[cell->texture].name, (unsigned)cell->slice, (unsigned)cell->level, (unsigned)x, (unsigned)y,
		(unsigned)stored, (unsigned)__builtin_bswap32(argb));
}

// The pixel at (x, y) is what it should be: the cell's texel inside it, the clear colour on the ring around it.
static bool pixel_right(
	const Cell *cell, volatile const uint32_t *pixels, uint32_t frame, uint32_t x, uint32_t y, uint32_t *logged)
{
	const bool inside = x >= cell->x && x < cell->x + cell->size && y >= cell->y && y < cell->y + cell->size;
	const uint32_t argb = inside ? expected_argb(cell, x - cell->x, y - cell->y) : CLEAR_ARGB;
	const uint32_t stored = pixels[tiled_index(x, y, WIDTH)];
	if (holds(stored, argb, 0))
		return true;
	if ((*logged)++ < MAX_MISSES_LOGGED)
		log_miss(cell, frame, x, y, stored, argb);
	return false;
}

// Every pixel of each cell and the ring around it, the wrong ones added to their group's.
static void count_wrong(const Cell *cells, uint32_t cell_count, volatile const uint32_t *pixels, uint32_t frame,
	uint32_t wrong[GROUPS], uint32_t *logged)
{
	for (uint32_t i = 0; i < cell_count; i++) {
		const Cell *const cell = &cells[i];
		for (uint32_t y = cell->y - 1; y <= cell->y + cell->size; y++)
			for (uint32_t x = cell->x - 1; x <= cell->x + cell->size; x++)
				wrong[cell->group] += !pixel_right(cell, pixels, frame, x, y, logged);
	}
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

// The coordinate at the cell's corner s across and t down, each 0 or 1: a cube's direction toward its face's sc and tc,
// as Vulkan's cube map face selection takes them back (Vulkan 1.0, "Cube map face selection"); a 3D texture's u, v and
// the cell's w.
static void corner_coord(const Cell *cell, float s, float t, float out[4])
{
	const float sc = 2.0f * s - 1.0f;
	const float tc = 2.0f * t - 1.0f;
	const float directions[XSDK_CUBE_FACES][4] = {
		{ 1.0f, -tc, -sc, 0.0f },
		{ -1.0f, -tc, sc, 0.0f },
		{ sc, 1.0f, tc, 0.0f },
		{ sc, -1.0f, -tc, 0.0f },
		{ sc, -tc, 1.0f, 0.0f },
		{ -sc, -tc, -1.0f, 0.0f },
	};
	const float volume[4] = { s, t, cell->w, 0.0f };
	const bool cube = specs[cell->texture].dimension == XSDK_TEXTURE_CUBE;
	memcpy(out, cube ? directions[cell->slice] : volume, sizeof volume);
}

// Two triangles, edges half a pixel before the pixels, for Direct3D centres (xenia:src/xenia/gpu/draw_util.cc:837).
static void put_quad(volatile uint32_t *vertices, const Cell *cell)
{
	static const float corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
	float quad[QUAD_VERTICES][VERTEX_FLOATS];
	for (uint32_t i = 0; i < QUAD_VERTICES; i++) {
		const float s = corners[i][0];
		const float t = corners[i][1];
		quad[i][0] = clip_x((float)cell->x + s * (float)cell->size - 0.5f);
		quad[i][1] = clip_y((float)cell->y + t * (float)cell->size - 0.5f);
		quad[i][2] = 0.0f;
		quad[i][3] = 1.0f;
		corner_coord(cell, s, t, &quad[i][4]);
	}
	uint32_t words[sizeof quad / 4];
	memcpy(words, quad, sizeof quad);
	for (uint32_t i = 0; i < sizeof quad / 4; i++)
		vertices[i] = words[i];
}

typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *cube;
	const XsdkShader *volume;
	const XsdkTexture *textures;
	const Cell *cells;
	const XsdkSamplerState *samplers;
	uint32_t cell_count;
	uint32_t vertices;
	volatile uint32_t *const *buffers;
	bool has_display;
} Scene;

static void set_literals(const VolumeShaderLiteral *literals, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
		xsdk_gpu_set_pixel_constants(literals[i].reg, literals[i].values, 1);
}

// The cube cells first, then the 3D ones, each with its pixel shader and the literals it pools.
static void draw_cells(const Scene *scene)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	const XsdkShader *pixel = NULL;
	for (uint32_t i = 0; i < scene->cell_count; i++) {
		const Cell *const cell = &scene->cells[i];
		const bool cube = specs[cell->texture].dimension == XSDK_TEXTURE_CUBE;
		if (pixel != (cube ? scene->cube : scene->volume)) {
			pixel = cube ? scene->cube : scene->volume;
			xsdk_gpu_set_shaders(scene->vertex, pixel);
			set_literals(cube ? cube_pixel_shader_literals : volume_pixel_shader_literals,
				cube ? CUBE_PIXEL_SHADER_LITERALS : VOLUME_PIXEL_SHADER_LITERALS);
		}
		xsdk_gpu_set_vertices(0, scene->vertices + i * QUAD_BYTES, QUAD_BYTES);
		xsdk_gpu_set_texture(TEXTURE_SLOT, &scene->textures[cell->texture], &scene->samplers[i]);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
}

static uint32_t make_points(const Cell *cells, uint32_t cell_count, Point *points)
{
	uint32_t count = 0;
	for (uint32_t i = 0; i < cell_count; i++) {
		const uint32_t last = cells[i].size - 1;
		const uint32_t places[POINTS_PER_CELL][2] = { { 0, 0 }, { last, 0 }, { 0, last }, { last, last },
			{ cells[i].size / 2, cells[i].size / 2 } };
		for (uint32_t j = 0; j < POINTS_PER_CELL; j++)
			points[count++] = (Point){ i, cells[i].x + places[j][0], cells[i].y + places[j][1] };
	}
	return count;
}

// Each frame draws every cell into eDRAM, resolves into the buffer not on screen, and shows it; its points are checked
// every frame after the first, and every pixel in the last.
static void draw_frames(const Scene *scene)
{
	static Point points[MAX_CELLS * POINTS_PER_CELL];
	const uint32_t point_count = make_points(scene->cells, scene->cell_count, points);
	uint32_t wrong[GROUPS] = { 0 };
	uint32_t logged = 0;
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = scene->buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		draw_cells(scene);
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		// The first frame's background is whatever eDRAM held; later ones are the clear colour.
		for (uint32_t i = 0; i < point_count && frame > 0 && fences_landed; i++) {
			const Cell *const cell = &scene->cells[points[i].cell];
			wrong[cell->group] += !pixel_right(cell, buffer, frame, points[i].x, points[i].y, &logged);
		}
		if (frame == FRAMES - 1 && fences_landed)
			count_wrong(scene->cells, scene->cell_count, buffer, frame, wrong, &logged);
		if (scene->has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	check("cube and 3D: every frame's fence landed", fences_landed);
	for (uint32_t i = 0; i < GROUPS; i++) {
		if (wrong[i])
			xsdk_log_write("cube and 3D: %u pixels wrong for \"%s\"", (unsigned)wrong[i], group_checks[i]);
		check(group_checks[i], fences_landed && wrong[i] == 0);
	}
	if (scene->has_display) {
		check("cube and 3D: every frame shown", shown);
		xsdk_log_write("cube and 3D: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
}

static bool load_shaders(XsdkShader *vertex, XsdkShader *cube, XsdkShader *volume)
{
	return NT_SUCCESS(xsdk_shader_load(vertex, volume_vertex_shader, sizeof volume_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(cube, cube_pixel_shader, sizeof cube_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(volume, volume_pixel_shader, sizeof volume_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			vertex, vertex_layout, sizeof vertex_layout / sizeof vertex_layout[0], (const uint32_t[]){ STRIDE_BYTES }));
}

void test_cube_volume(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("cube and 3D: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("cube and 3D: display open at 1280x720", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader cube = { .microcode = NULL };
	XsdkShader volume = { .microcode = NULL };
	const bool loaded = load_shaders(&vertex, &cube, &volume);
	check("cube and 3D: xost's shaders load, the vertex fetches patched for two float4s", loaded);
	static XsdkTexture textures[TEXTURES];
	bool made = true;
	for (uint32_t i = 0; i < TEXTURES; i++)
		made = make_texture(&textures[i], i) && made;
	check("cube and 3D: two cubes and two 3D textures made, uploaded face by face and slice by slice", made);
	bool same = made;
	for (uint32_t i = 0; i < TEXTURES && made; i++)
		same = reads_back(&textures[i], i) && same;
	check("cube and 3D: every face and slice of every level reads back as uploaded", same);
	static Cell cells[MAX_CELLS];
	const uint32_t cell_count = make_cells(cells);
	static XsdkSamplerState samplers[MAX_CELLS];
	for (uint32_t i = 0; i < cell_count; i++)
		xsdk_texture_sampler_state(&samplers[i], &cells[i].sampler);
	volatile uint32_t *const vertices = xsdk_gpu_alloc(cell_count * QUAD_BYTES);
	if (vertices) {
		for (uint32_t i = 0; i < cell_count; i++)
			put_quad(vertices + i * QUAD_BYTES / 4, &cells[i]);
		__asm__ volatile("sync" ::: "memory");
		xsdk_gpu_invalidate(xsdk_gpu_address(vertices), cell_count * QUAD_BYTES);
	}
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("cube and 3D: vertex and front buffers", vertices && buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (loaded && made && vertices && buffers[0] && buffers[1]) {
		const Scene scene = { .vertex = &vertex,
			.cube = &cube,
			.volume = &volume,
			.textures = textures,
			.cells = cells,
			.samplers = samplers,
			.cell_count = cell_count,
			.vertices = xsdk_gpu_address(vertices),
			.buffers = buffers,
			.has_display = has_display };
		draw_frames(&scene);
	}
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	for (uint32_t i = 0; i < TEXTURES; i++)
		if (textures[i].texels)
			xsdk_texture_free(&textures[i]);
	xsdk_shader_free(&volume);
	xsdk_shader_free(&cube);
	xsdk_shader_free(&vertex);
	check("cube and 3D: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
