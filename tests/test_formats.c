#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// One quad per format, linear and tiled, and every level of six formats, each checked against a decode on the CPU.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 60,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 8,
	CLEAR_ARGB = 0xFF202020,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	TEXCOORD_OFFSET_BYTES = 16,
	TEXTURE_SLOT = 0,
	TEXTURE_SIZE = 32,
	MIP_LEVELS = 6,
	// Two pixels a texel.
	CELL_SIZE = 64,
	// Sixteen formats across a 1280-pixel row.
	CELL_STEP = 76,
	LEVEL_GAP = 12,
	MIP_ROW_STEP = 48,
	// Fixed-point channels convert to float and round once more on the way to the 8-bit target.
	UNORM_TOLERANCE = 1,
	// Decoders round DXT's blends their own way: Xenia's host gave 0xAF for two thirds of full green, not 0xAA. Still
	// well under the 85 between neighbouring blends of these blocks, so a wrong index or block fails.
	DXT_BLEND_TOLERANCE = 12,
	MAX_CELLS = 72,
	SWIZZLE_ROW_Y = 600,
	// The upload timed: an R8 tiled texture this many texels on a side, in under this long.
	TIMED_SIZE = 512,
	TIMED_LIMIT_US = 4000,
	POINTS_PER_CELL = 10,
	// The largest level: 32x32 texels of 8 bytes.
	MAX_LEVEL_BYTES = TEXTURE_SIZE * TEXTURE_SIZE * 8,
	MAX_BLOCK_BYTES = 16,
	CHANNELS = 4,
	ALPHA = 3,
};

enum {
	GROUP_FORMATS,
	GROUP_MIPS,
	GROUP_SWIZZLES,
	GROUPS,
};

enum {
	// One unit of 2 or 4 bytes, each channel at a shift.
	KIND_PACKED,
	// One byte a channel, each at its shift's byte.
	KIND_BYTES,
	// One half float a channel, red first.
	KIND_HALVES,
	// One float a channel, red first.
	KIND_FLOATS,
	KIND_DXT1,
	KIND_DXT3,
	KIND_DXT5,
};

typedef struct {
	const char *name;
	uint32_t kind;
	// Bytes in a texel, or in a 4 by 4 block.
	uint32_t block_bytes;
	// Bits in red, green, blue and alpha; 0 for none.
	uint8_t bits[CHANNELS];
	// Where each sits in a packed unit, or which byte holds it.
	uint8_t shifts[CHANNELS];
} FormatSpec;

static const FormatSpec specs[XSDK_FORMATS] = {
	[XSDK_FORMAT_A8R8G8B8] = { "A8R8G8B8", KIND_PACKED, 4, { 8, 8, 8, 8 }, { 16, 8, 0, 24 } },
	[XSDK_FORMAT_R5G6B5] = { "R5G6B5", KIND_PACKED, 2, { 5, 6, 5, 0 }, { 11, 5, 0, 0 } },
	[XSDK_FORMAT_A1R5G5B5] = { "A1R5G5B5", KIND_PACKED, 2, { 5, 5, 5, 1 }, { 10, 5, 0, 15 } },
	[XSDK_FORMAT_A4R4G4B4] = { "A4R4G4B4", KIND_PACKED, 2, { 4, 4, 4, 4 }, { 8, 4, 0, 12 } },
	[XSDK_FORMAT_A2B10G10R10] = { "A2B10G10R10", KIND_PACKED, 4, { 10, 10, 10, 2 }, { 0, 10, 20, 30 } },
	[XSDK_FORMAT_R8] = { "R8", KIND_BYTES, 1, { 8, 0, 0, 0 }, { 0 } },
	[XSDK_FORMAT_R8G8] = { "R8G8", KIND_BYTES, 2, { 8, 8, 0, 0 }, { 0, 1 } },
	[XSDK_FORMAT_R16F] = { "R16F", KIND_HALVES, 2, { 16, 0, 0, 0 }, { 0 } },
	[XSDK_FORMAT_R16G16F] = { "R16G16F", KIND_HALVES, 4, { 16, 16, 0, 0 }, { 0 } },
	[XSDK_FORMAT_R16G16B16A16F] = { "R16G16B16A16F", KIND_HALVES, 8, { 16, 16, 16, 16 }, { 0 } },
	[XSDK_FORMAT_DXT1] = { "DXT1", KIND_DXT1, 8, { 0 }, { 0 } },
	[XSDK_FORMAT_DXT3] = { "DXT3", KIND_DXT3, 16, { 0 }, { 0 } },
	[XSDK_FORMAT_DXT5] = { "DXT5", KIND_DXT5, 16, { 0 }, { 0 } },
	[XSDK_FORMAT_R8G8B8A8] = { "R8G8B8A8", KIND_BYTES, 4, { 8, 8, 8, 8 }, { 0, 1, 2, 3 } },
	[XSDK_FORMAT_B8G8R8A8] = { "B8G8R8A8", KIND_BYTES, 4, { 8, 8, 8, 8 }, { 2, 1, 0, 3 } },
	[XSDK_FORMAT_R32F] = { "R32F", KIND_FLOATS, 4, { 32, 0, 0, 0 }, { 0 } },
};

// One format per kind of block: 1, 2, 4 and 8 bytes a texel, and 8 and 16 bytes a 4 by 4 block.
static const uint32_t mipped_formats[] = { XSDK_FORMAT_R8, XSDK_FORMAT_R5G6B5, XSDK_FORMAT_A2B10G10R10,
	XSDK_FORMAT_R16G16B16A16F, XSDK_FORMAT_DXT1, XSDK_FORMAT_DXT5 };

// Textures drawn with their channels picked (xsdk_texture_swizzle): R8 as intensity, R8G8 as intensity and alpha.
typedef struct {
	const char *name;
	uint32_t format;
	uint32_t channels[4];
} Swizzled;

static const Swizzled swizzled[] = {
	{ "R8 as RRRR", XSDK_FORMAT_R8, { XSDK_CHANNEL_RED, XSDK_CHANNEL_RED, XSDK_CHANNEL_RED, XSDK_CHANNEL_RED } },
	{ "R8G8 as RRRG", XSDK_FORMAT_R8G8, { XSDK_CHANNEL_RED, XSDK_CHANNEL_RED, XSDK_CHANNEL_RED, XSDK_CHANNEL_GREEN } },
};

enum {
	MIPPED = sizeof mipped_formats / sizeof mipped_formats[0],
	SWIZZLED = sizeof swizzled / sizeof swizzled[0],
	// Each format linear, then tiled, then the mipped ones, then the swizzled ones.
	FIRST_MIPPED = XSDK_FORMATS * 2,
	FIRST_SWIZZLED = FIRST_MIPPED + MIPPED,
	TEXTURES = FIRST_SWIZZLED + SWIZZLED,
};

// Exact half floats, as bits and as values; above 1 and below 0 clamp in the 8-bit target.
static const uint16_t half_bits[] = { 0x0000, 0x2C00, 0x3000, 0x3400, 0x3600, 0x3900, 0x3A00, 0x3B00, 0x3C00, 0x4000,
	0xBC00 };
enum {
	HALF_STEPS = 8,
	HALF_ABOVE_ONE = 9,
	HALF_BELOW_ZERO = 10,
};

// The corners of 5-6-5 colour: every channel empty or full, so DXT's thirds come out whole.
static const uint16_t corners[8] = { 0x0000, 0x001F, 0x07E0, 0x07FF, 0xF800, 0xF81F, 0xFFE0, 0xFFFF };

typedef struct {
	uint32_t texture;
	uint32_t format;
	uint32_t group;
	uint32_t level;
	bool tiled;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	// Its entry in swizzled, for a cell of GROUP_SWIZZLES.
	const Swizzled *swizzle;
} Cell;

typedef struct {
	uint32_t cell;
	uint32_t x;
	uint32_t y;
	uint32_t argb;
	uint32_t tolerance;
} Point;

static const XsdkVertexElement vertex_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.usage_index = 0,
		.format = XSDK_VERTEX_FLOAT2,
		.offset_bytes = TEXCOORD_OFFSET_BYTES },
};

static uint8_t level_data[MAX_LEVEL_BYTES];

static uint32_t level_size(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

static bool is_dxt(uint32_t format)
{
	return specs[format].kind >= KIND_DXT1;
}

static uint32_t blocks(uint32_t texels, uint32_t format)
{
	return is_dxt(format) ? (texels + 3) / 4 : texels;
}

// A channel from 0 to 1: across, down, a checker or a diagonal, mixed differently for each format and level.
static float pattern(uint32_t format, uint32_t level, uint32_t channel, uint32_t x, uint32_t y)
{
	const uint32_t last = level_size(TEXTURE_SIZE, level) > 1 ? level_size(TEXTURE_SIZE, level) - 1 : 1;
	const uint32_t square = 1 + (format + level) % 3;
	switch ((channel + format + level) % 4) {
	case 0:
		return (float)x / (float)last;
	case 1:
		return (float)y / (float)last;
	case 2:
		return (float)((x >> square ^ y >> square) & 1);
	default:
		return (float)(x + y) / (float)(2 * last);
	}
}

static bool is_checker(uint32_t format, uint32_t level, uint32_t channel)
{
	return (channel + format + level) % 4 == 2;
}

static uint32_t quantise(float value, uint32_t bits)
{
	return (uint32_t)(value * (float)((1u << bits) - 1) + 0.5f);
}

// The checker's halves go past 0 and 1, so the 8-bit target clamps them.
static uint16_t half_for(uint32_t format, uint32_t level, uint32_t channel, uint32_t x, uint32_t y)
{
	const float value = pattern(format, level, channel, x, y);
	if (is_checker(format, level, channel))
		return half_bits[value > 0.5f ? HALF_ABOVE_ONE : HALF_BELOW_ZERO];
	return half_bits[(uint32_t)(value * HALF_STEPS + 0.5f)];
}

static void put_u16(uint8_t *out, uint16_t value)
{
	out[0] = (uint8_t)(value >> 8);
	out[1] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *in)
{
	return (uint16_t)(in[0] << 8 | in[1]);
}

// A float's bits as the CPU holds them, first byte highest.
static void put_float(uint8_t *out, float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	for (uint32_t i = 0; i < 4; i++)
		out[i] = (uint8_t)(bits >> (24 - 8 * i));
}

static float get_float(const uint8_t *in)
{
	const uint32_t bits = (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
	float value;
	memcpy(&value, &bits, sizeof value);
	return value;
}

// Endpoints from the corners and two index bits a texel; three_colour orders them for DXT1's transparent mode.
static void colour_block(uint32_t format, uint32_t level, uint32_t bx, uint32_t by, bool three_colour, uint8_t *out)
{
	const uint16_t first = corners[(bx + by * 3 + format + level) % 8];
	const uint16_t picked = corners[(bx * 5 + by + 3 + level) % 8];
	const uint16_t second = picked == first ? (uint16_t)~first : picked;
	const uint16_t high = first > second ? first : second;
	const uint16_t low = first > second ? second : first;
	uint32_t indices = 0;
	for (uint32_t texel = 0; texel < 16; texel++)
		indices |= ((texel % 4 + 2 * (texel / 4) + bx + level) & 3) << (2 * texel);
	put_u16(out, three_colour ? low : high);
	put_u16(out + 2, three_colour ? high : low);
	put_u16(out + 4, (uint16_t)indices);
	put_u16(out + 6, (uint16_t)(indices >> 16));
}

static void dxt3_alpha(uint32_t level, uint32_t bx, uint32_t by, uint8_t *out)
{
	for (uint32_t word = 0; word < 4; word++) {
		uint32_t value = 0;
		for (uint32_t i = 0; i < 4; i++) {
			const uint32_t texel = word * 4 + i;
			value |= ((texel % 4 * 4 + texel / 4 + bx + by + level) & 15) << (4 * i);
		}
		put_u16(out + 2 * word, (uint16_t)value);
	}
}

// Eight alphas when the first is higher, else six and 0 and 255.
static void dxt5_alpha(uint32_t level, uint32_t bx, uint32_t by, uint8_t *out)
{
	const uint32_t high = 0xF0 - (bx % 4) * 0x10;
	const uint32_t low = (by % 4) * 0x10;
	const bool eight = (bx + by + level) % 2 == 0;
	uint64_t indices = 0;
	for (uint32_t texel = 0; texel < 16; texel++)
		indices |= (uint64_t)((texel % 4 + texel / 4 * 3 + bx + level) & 7) << (3 * texel);
	put_u16(out, (uint16_t)(eight ? high | low << 8 : low | high << 8));
	put_u16(out + 2, (uint16_t)indices);
	put_u16(out + 4, (uint16_t)(indices >> 16));
	put_u16(out + 6, (uint16_t)(indices >> 32));
}

static float half_value(uint16_t bits)
{
	const uint32_t exponent = bits >> 10 & 0x1F;
	const float magnitude =
		exponent ? (float)(0x400 | (bits & 0x3FF)) * (float)(1u << exponent) / (float)(1u << 25) : 0.0f;
	return bits & 0x8000 ? -magnitude : magnitude;
}

// A texel, or a 4 by 4 block, in the units xsdk_texture_upload takes.
static void encode_block(uint32_t format, uint32_t level, uint32_t bx, uint32_t by, uint8_t *out)
{
	const FormatSpec *const spec = &specs[format];
	if (spec->kind == KIND_PACKED) {
		uint32_t unit = 0;
		for (uint32_t channel = 0; channel < CHANNELS; channel++)
			if (spec->bits[channel])
				unit |= quantise(pattern(format, level, channel, bx, by), spec->bits[channel]) << spec->shifts[channel];
		for (uint32_t i = 0; i < spec->block_bytes; i++)
			out[i] = (uint8_t)(unit >> (8 * (spec->block_bytes - 1 - i)));
	} else if (spec->kind == KIND_BYTES) {
		for (uint32_t channel = 0; channel < CHANNELS; channel++)
			if (spec->bits[channel])
				out[spec->shifts[channel]] = (uint8_t)quantise(pattern(format, level, channel, bx, by), 8);
	} else if (spec->kind == KIND_HALVES) {
		for (uint32_t channel = 0; channel < spec->block_bytes / 2; channel++)
			put_u16(out + 2 * channel, half_for(format, level, channel, bx, by));
	} else if (spec->kind == KIND_FLOATS) {
		// The halves' values, past 0 and 1 included.
		for (uint32_t channel = 0; channel < spec->block_bytes / 4; channel++)
			put_float(out + 4 * channel, half_value(half_for(format, level, channel, bx, by)));
	} else if (spec->kind == KIND_DXT1) {
		colour_block(format, level, bx, by, (bx + by + level) % 2 == 1, out);
	} else {
		if (spec->kind == KIND_DXT3)
			dxt3_alpha(level, bx, by, out);
		else
			dxt5_alpha(level, bx, by, out);
		colour_block(format, level, bx, by, false, out + 8);
	}
}

static uint32_t to_byte(float value)
{
	const float rounded = value + 0.5f;
	return rounded < 0.0f ? 0 : rounded >= 255.0f ? 255 : (uint32_t)rounded;
}

static void expand_565(uint16_t colour, float out[3])
{
	out[0] = (float)(colour >> 11) * 255.0f / 31.0f;
	out[1] = (float)(colour >> 5 & 0x3F) * 255.0f / 63.0f;
	out[2] = (float)(colour & 0x1F) * 255.0f / 31.0f;
}

// Red, green, blue and alpha of a texel in a colour block, alpha 0 for DXT1's transparent black; returns whether it's
// a blend.
static bool decode_colour(const uint8_t *block, uint32_t texel, bool dxt1, float out[CHANNELS])
{
	const uint16_t first = get_u16(block);
	const uint16_t second = get_u16(block + 2);
	const uint32_t indices = get_u16(block + 4) | (uint32_t)get_u16(block + 6) << 16;
	const uint32_t index = indices >> (2 * texel) & 3;
	const bool three_colour = dxt1 && first <= second;
	float ends[2][3];
	expand_565(first, ends[0]);
	expand_565(second, ends[1]);
	out[ALPHA] = three_colour && index == 3 ? 0.0f : 255.0f;
	for (uint32_t channel = 0; channel < 3; channel++) {
		const float a = ends[0][channel];
		const float b = ends[1][channel];
		const float blends[4] = { a, b, three_colour ? (a + b) / 2.0f : (2.0f * a + b) / 3.0f,
			three_colour ? 0.0f : (a + 2.0f * b) / 3.0f };
		out[channel] = blends[index];
	}
	return index == 2 || (index == 3 && !three_colour);
}

// The texel's alpha; blended is set when it lies between the ends.
static float dxt5_alpha_value(const uint8_t *block, uint32_t texel, bool *blended)
{
	const uint16_t ends = get_u16(block);
	const float first = (float)(ends & 0xFF);
	const float second = (float)(ends >> 8);
	const uint64_t indices =
		get_u16(block + 2) | (uint64_t)get_u16(block + 4) << 16 | (uint64_t)get_u16(block + 6) << 32;
	const uint32_t index = (uint32_t)(indices >> (3 * texel) & 7);
	*blended = index >= 2 && (first > second || index < 6);
	if (index < 2)
		return index ? second : first;
	if (first > second)
		return ((float)(8 - index) * first + (float)(index - 1) * second) / 7.0f;
	if (index >= 6)
		return index == 6 ? 0.0f : 255.0f;
	return ((float)(6 - index) * first + (float)(index - 1) * second) / 5.0f;
}

// What the GPU should draw for a texel: the format's channels as 8-bit ARGB, missing ones 0 and alpha 255.
static uint32_t decode_texel(uint32_t format, uint32_t level, uint32_t x, uint32_t y, uint32_t *tolerance)
{
	const FormatSpec *const spec = &specs[format];
	uint8_t block[MAX_BLOCK_BYTES];
	encode_block(format, level, is_dxt(format) ? x / 4 : x, is_dxt(format) ? y / 4 : y, block);
	float channels[CHANNELS] = { 0.0f, 0.0f, 0.0f, 255.0f };
	bool blended = false;
	if (spec->kind == KIND_PACKED) {
		uint32_t unit = 0;
		for (uint32_t i = 0; i < spec->block_bytes; i++)
			unit = unit << 8 | block[i];
		for (uint32_t channel = 0; channel < CHANNELS; channel++) {
			const uint32_t max = (1u << spec->bits[channel]) - 1;
			if (spec->bits[channel])
				channels[channel] = (float)(unit >> spec->shifts[channel] & max) * 255.0f / (float)max;
		}
	} else if (spec->kind == KIND_BYTES) {
		for (uint32_t channel = 0; channel < CHANNELS; channel++)
			if (spec->bits[channel])
				channels[channel] = (float)block[spec->shifts[channel]];
	} else if (spec->kind == KIND_HALVES) {
		for (uint32_t channel = 0; channel < spec->block_bytes / 2; channel++)
			channels[channel] = half_value(get_u16(block + 2 * channel)) * 255.0f;
	} else if (spec->kind == KIND_FLOATS) {
		for (uint32_t channel = 0; channel < spec->block_bytes / 4; channel++)
			channels[channel] = get_float(block + 4 * channel) * 255.0f;
	} else {
		const uint32_t texel = y % 4 * 4 + x % 4;
		blended = decode_colour(spec->kind == KIND_DXT1 ? block : block + 8, texel, spec->kind == KIND_DXT1, channels);
		bool alpha_blended = false;
		if (spec->kind == KIND_DXT3)
			channels[ALPHA] = (float)(get_u16(block + texel / 4 * 2) >> (texel % 4 * 4) & 15) * 17.0f;
		else if (spec->kind == KIND_DXT5)
			channels[ALPHA] = dxt5_alpha_value(block, texel, &alpha_blended);
		blended = blended || alpha_blended;
	}
	*tolerance = blended ? DXT_BLEND_TOLERANCE : UNORM_TOLERANCE;
	return to_byte(channels[ALPHA]) << 24 | to_byte(channels[0]) << 16 | to_byte(channels[1]) << 8 |
		to_byte(channels[2]);
}

// A decoded texel with its channels picked as the swizzle names them.
static uint32_t swizzle_argb(uint32_t argb, const uint32_t channels[4])
{
	const uint32_t source[4] = { argb >> 16 & 0xFF, argb >> 8 & 0xFF, argb & 0xFF, argb >> 24 };
	uint32_t out[4];
	for (uint32_t i = 0; i < 4; i++)
		out[i] = channels[i] == XSDK_CHANNEL_ZERO ? 0 : channels[i] == XSDK_CHANNEL_ONE ? 0xFF : source[channels[i]];
	return out[3] << 24 | out[0] << 16 | out[1] << 8 | out[2];
}

static uint32_t texture_format(uint32_t texture)
{
	if (texture < FIRST_MIPPED)
		return texture % XSDK_FORMATS;
	return texture < FIRST_SWIZZLED ? mipped_formats[texture - FIRST_MIPPED]
									: swizzled[texture - FIRST_SWIZZLED].format;
}

static bool make_texture(XsdkTexture *texture, uint32_t which)
{
	const uint32_t format = texture_format(which);
	const bool tiled = which >= XSDK_FORMATS;
	const uint32_t levels = which >= FIRST_MIPPED && which < FIRST_SWIZZLED ? MIP_LEVELS : 1;
	const NTSTATUS status = xsdk_texture_create(texture, format, TEXTURE_SIZE, TEXTURE_SIZE, levels, tiled);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("formats: %s: xsdk_texture_create failed: 0x%08X", specs[format].name, (unsigned)status);
		return false;
	}
	const uint32_t block_bytes = specs[format].block_bytes;
	for (uint32_t level = 0; level < levels; level++) {
		const uint32_t columns = blocks(level_size(TEXTURE_SIZE, level), format);
		const uint32_t rows = blocks(level_size(TEXTURE_SIZE, level), format);
		for (uint32_t by = 0; by < rows; by++)
			for (uint32_t bx = 0; bx < columns; bx++)
				encode_block(format, level, bx, by, level_data + (by * columns + bx) * block_bytes);
		xsdk_texture_upload(texture, level, level_data, columns * block_bytes);
		xsdk_gpu_invalidate(texture->address, texture->size_bytes);
	}
	if (which >= FIRST_SWIZZLED)
		xsdk_texture_swizzle(texture, swizzled[which - FIRST_SWIZZLED].channels);
	return true;
}

// Columns of formats, linear above tiled, then a row of levels for each mipped format, then the swizzled ones.
static uint32_t make_cells(Cell *cells)
{
	uint32_t count = 0;
	for (uint32_t texture = 0; texture < XSDK_FORMATS * 2; texture++) {
		const bool tiled = texture >= XSDK_FORMATS;
		cells[count++] = (Cell){ texture, texture % XSDK_FORMATS, GROUP_FORMATS, 0, tiled,
			40 + texture % XSDK_FORMATS * CELL_STEP, tiled ? 160 : 60, CELL_SIZE, CELL_SIZE, NULL };
	}
	for (uint32_t i = 0; i < MIPPED; i++) {
		uint32_t x = 40;
		for (uint32_t level = 0; level < MIP_LEVELS; level++) {
			const uint32_t size = level_size(TEXTURE_SIZE, level);
			cells[count++] = (Cell){ XSDK_FORMATS * 2 + i, mipped_formats[i], GROUP_MIPS, level, true, x,
				280 + i * MIP_ROW_STEP, size, size, NULL };
			x += size + LEVEL_GAP;
		}
	}
	for (uint32_t i = 0; i < SWIZZLED; i++)
		cells[count++] = (Cell){ FIRST_SWIZZLED + i, swizzled[i].format, GROUP_SWIZZLES, 0, true, 40 + i * CELL_STEP,
			SWIZZLE_ROW_Y, CELL_SIZE, CELL_SIZE, &swizzled[i] };
	return count;
}

// The texel under a pixel, point-sampled at the cell's level, and how far it may be off.
static uint32_t expected_argb(const Cell *cell, uint32_t x, uint32_t y, uint32_t *tolerance)
{
	const uint32_t size = level_size(TEXTURE_SIZE, cell->level);
	const uint32_t argb = decode_texel(
		cell->format, cell->level, (x - cell->x) * size / cell->width, (y - cell->y) * size / cell->height, tolerance);
	return cell->swizzle ? swizzle_argb(argb, cell->swizzle->channels) : argb;
}

static const char *cell_name(const Cell *cell)
{
	return cell->swizzle ? cell->swizzle->name : specs[cell->format].name;
}

static uint32_t make_points(const Cell *cells, uint32_t cell_count, Point *points)
{
	static const uint32_t quarters[POINTS_PER_CELL - 1][2] = { { 0, 0 }, { 4, 0 }, { 0, 4 }, { 4, 4 }, { 2, 2 },
		{ 1, 1 }, { 3, 1 }, { 1, 3 }, { 3, 3 } };
	uint32_t count = 0;
	for (uint32_t i = 0; i < cell_count; i++) {
		const Cell *const cell = &cells[i];
		for (uint32_t j = 0; j < POINTS_PER_CELL - 1; j++) {
			const uint32_t x = cell->x + (cell->width - 1) * quarters[j][0] / 4;
			const uint32_t y = cell->y + (cell->height - 1) * quarters[j][1] / 4;
			uint32_t tolerance;
			const uint32_t argb = expected_argb(cell, x, y, &tolerance);
			points[count++] = (Point){ i, x, y, argb, tolerance };
		}
		points[count++] = (Point){ i, cell->x + cell->width, cell->y + cell->height, CLEAR_ARGB, 0 };
	}
	return count;
}

// The largest difference in any channel, alpha too; a resolve stores the colour's bytes B, G, R, A.
static uint32_t error(uint32_t stored, uint32_t argb)
{
	const uint32_t want = __builtin_bswap32(argb);
	uint32_t largest = 0;
	for (uint32_t shift = 0; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		const uint32_t difference = got > wanted ? got - wanted : wanted - got;
		largest = difference > largest ? difference : largest;
	}
	return largest;
}

static void log_miss(const Cell *cell, uint32_t frame, uint32_t x, uint32_t y, uint32_t stored, uint32_t argb)
{
	xsdk_log_write("formats: frame %u: %s %s level %u at (%u, %u) held %08X, not %08X", (unsigned)frame,
		cell_name(cell), cell->tiled ? "tiled" : "linear", (unsigned)cell->level, (unsigned)x, (unsigned)y,
		(unsigned)stored, (unsigned)__builtin_bswap32(argb));
}

// Every pixel of the cell and the ring around it; returns how many are wrong, and raises the format's largest error.
static uint32_t cell_wrong(
	const Cell *cell, volatile const uint32_t *pixels, uint32_t frame, uint32_t *logged, uint32_t *largest)
{
	uint32_t wrong = 0;
	for (uint32_t y = cell->y - 1; y <= cell->y + cell->height; y++) {
		for (uint32_t x = cell->x - 1; x <= cell->x + cell->width; x++) {
			const bool inside = x >= cell->x && x < cell->x + cell->width && y >= cell->y && y < cell->y + cell->height;
			uint32_t tolerance = 0;
			const uint32_t argb = inside ? expected_argb(cell, x, y, &tolerance) : CLEAR_ARGB;
			const uint32_t stored = pixels[tiled_index(x, y, WIDTH)];
			const uint32_t off = error(stored, argb);
			if (inside && off > *largest)
				*largest = off;
			if (off <= tolerance)
				continue;
			if ((*logged)++ < MAX_MISSES_LOGGED)
				log_miss(cell, frame, x, y, stored, argb);
			wrong++;
		}
	}
	return wrong;
}

// Each format's largest error in any channel, on one line.
static void log_largest(const uint32_t largest[XSDK_FORMATS])
{
	char line[256];
	uint32_t length = 0;
	for (uint32_t format = 0; format < XSDK_FORMATS && length < sizeof line; format++)
		length += (uint32_t)snprintf(line + length, sizeof line - length, "%s%s %u", format ? ", " : "",
			specs[format].name, (unsigned)largest[format]);
	xsdk_log_write("formats: the largest error in a channel: %s", line);
}

// Adds each group's wrong pixels, naming each cell with any; returns how many pixels were checked.
static uint32_t count_wrong(
	const Cell *cells, uint32_t cell_count, volatile const uint32_t *pixels, uint32_t frame, uint32_t wrong[GROUPS])
{
	uint32_t checked = 0;
	uint32_t logged = 0;
	uint32_t largest[XSDK_FORMATS] = { 0 };
	for (uint32_t i = 0; i < cell_count; i++) {
		const Cell *const cell = &cells[i];
		const uint32_t ring = (cell->width + 2) * (cell->height + 2);
		const uint32_t cell_misses = cell_wrong(cell, pixels, frame, &logged, &largest[cell->format]);
		checked += ring;
		wrong[cell->group] += cell_misses;
		if (cell_misses)
			xsdk_log_write("formats: %s %s level %u: %u of %u pixels wrong", cell_name(cell),
				cell->tiled ? "tiled" : "linear", (unsigned)cell->level, (unsigned)cell_misses, (unsigned)ring);
	}
	log_largest(largest);
	return checked;
}

static float clip_x(float x)
{
	return x / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(float y)
{
	return 1.0f - y / (HEIGHT / 2.0f);
}

// Two triangles, edges half a pixel before the pixels, for Direct3D centres (xenia:src/xenia/gpu/draw_util.cc:837).
static void put_quad(volatile uint32_t *vertices, const Cell *cell)
{
	const float left = clip_x(cell->x - 0.5f);
	const float right = clip_x(cell->x + cell->width - 0.5f);
	const float top = clip_y(cell->y - 0.5f);
	const float bottom = clip_y(cell->y + cell->height - 0.5f);
	const float quad[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, 0.0f, 1.0f, 0.0f, 0.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
	};
	uint32_t words[sizeof quad / 4];
	memcpy(words, quad, sizeof quad);
	for (uint32_t i = 0; i < sizeof quad / 4; i++)
		vertices[i] = words[i];
}

typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *pixel;
	const XsdkTexture *textures;
	const XsdkSamplerState *sampler;
	const Cell *cells;
	uint32_t cell_count;
	uint32_t vertices;
	volatile uint32_t *const *buffers;
	bool has_display;
} Scene;

static void draw_cells(const Scene *scene)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(scene->vertex, scene->pixel);
	for (uint32_t i = 0; i < scene->cell_count; i++) {
		xsdk_gpu_set_vertices(0, scene->vertices + i * QUAD_BYTES, QUAD_BYTES);
		xsdk_gpu_set_texture(TEXTURE_SLOT, &scene->textures[scene->cells[i].texture], scene->sampler);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
}

static const char *const group_checks[GROUPS] = {
	"formats: every format, linear and tiled, as decoded on the CPU",
	"formats: every level of six formats' mipped textures, as decoded on the CPU",
	"formats: R8 and R8G8 with their channels picked, as decoded on the CPU",
};

// Each frame draws every cell into eDRAM, resolves into the buffer not on screen, and shows it.
static void draw_frames(const Scene *scene)
{
	static Point points[MAX_CELLS * POINTS_PER_CELL];
	const uint32_t point_count = make_points(scene->cells, scene->cell_count, points);
	uint32_t misses = 0;
	uint32_t frames_wrong = 0;
	uint32_t wrong[GROUPS] = { 0 };
	uint32_t checked = 0;
	uint32_t pixels_wrong = 0;
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = scene->buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		draw_cells(scene);
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		if (!fences_landed)
			xsdk_log_write("formats: frame %u's fence didn't land", (unsigned)frame);
		// The first frame's background is whatever eDRAM held; later ones are the clear colour.
		if (frame > 0 && fences_landed) {
			bool frame_holds = true;
			for (uint32_t i = 0; i < point_count; i++) {
				const Point *const p = &points[i];
				const Cell *const cell = &scene->cells[p->cell];
				const uint32_t stored = buffer[tiled_index(p->x, p->y, WIDTH)];
				if (error(stored, p->argb) <= p->tolerance)
					continue;
				if (misses++ < MAX_MISSES_LOGGED)
					log_miss(cell, frame, p->x, p->y, stored, p->argb);
				wrong[cell->group]++;
				frame_holds = false;
			}
			frames_wrong += !frame_holds;
		}
		if (frame == FRAMES - 1 && fences_landed) {
			uint32_t last_wrong[GROUPS] = { 0 };
			checked = count_wrong(scene->cells, scene->cell_count, buffer, frame, last_wrong);
			for (uint32_t i = 0; i < GROUPS; i++) {
				pixels_wrong += last_wrong[i];
				wrong[i] += last_wrong[i];
			}
		}
		if (scene->has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write("formats: %u cells, %u points a frame; %u of %u frames checked held a wrong one, %u points wrong",
		(unsigned)scene->cell_count, (unsigned)point_count, (unsigned)frames_wrong, (unsigned)(FRAMES - 1),
		(unsigned)misses);
	xsdk_log_write("formats: the last frame had %u of %u pixels wrong across the cells and their edges",
		(unsigned)pixels_wrong, (unsigned)checked);
	check("formats: every frame's fence landed", fences_landed);
	for (uint32_t i = 0; i < GROUPS; i++)
		check(group_checks[i], fences_landed && checked > 0 && wrong[i] == 0);
	if (scene->has_display) {
		check("formats: every frame shown", shown);
		xsdk_log_write("formats: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
}

static int64_t now_us(void)
{
	struct timespec time;
	(void)clock_gettime(CLOCK_MONOTONIC, &time);
	return (int64_t)time.tv_sec * 1000000 + time.tv_nsec / 1000;
}

// An R8 tiled upload from cached memory, timed. Written a byte at a time, each merged into its word read back from
// write-combined memory, it took an uncached read a texel; in runs of whole words it takes far less.
static void check_upload_time(void)
{
	XsdkTexture texture;
	const NTSTATUS status = xsdk_texture_create(&texture, XSDK_FORMAT_R8, TIMED_SIZE, TIMED_SIZE, 1, true);
	uint8_t *const data = malloc(TIMED_SIZE * TIMED_SIZE);
	if (!NT_SUCCESS(status) || !data) {
		check("formats: a 512x512 R8 tiled upload takes under 4 ms", false);
		free(data);
		if (NT_SUCCESS(status))
			xsdk_texture_free(&texture);
		return;
	}
	for (uint32_t i = 0; i < TIMED_SIZE * TIMED_SIZE; i++)
		data[i] = (uint8_t)(i * 7);
	const int64_t start = now_us();
	xsdk_texture_upload(&texture, 0, data, TIMED_SIZE);
	const int64_t elapsed = now_us() - start;
	xsdk_log_write("formats: a 512x512 R8 tiled upload took %u us, %u MB/s", (unsigned)elapsed,
		(unsigned)(elapsed > 0 ? (int64_t)TIMED_SIZE * TIMED_SIZE / elapsed : 0));
	check("formats: a 512x512 R8 tiled upload takes under 4 ms", elapsed < TIMED_LIMIT_US);
	free(data);
	xsdk_texture_free(&texture);
}

void test_formats(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("formats: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("formats: display open at 1280x720", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, texture_vertex_shader, sizeof texture_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, texture_pixel_shader, sizeof texture_pixel_shader));
	check("formats: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex, vertex_layout, sizeof vertex_layout / sizeof vertex_layout[0],
			(const uint32_t[]){ STRIDE_BYTES }));
	check("formats: vertex fetches patched for a float4 position and float2 coordinates", patched);
	static XsdkTexture textures[TEXTURES];
	bool made = true;
	for (uint32_t i = 0; i < TEXTURES; i++)
		made = make_texture(&textures[i], i) && made;
	check("formats: textures made in every format", made);
	check_upload_time();
	static Cell cells[MAX_CELLS];
	const uint32_t cell_count = make_cells(cells);
	const XsdkSampler point_clamped = { XSDK_FILTER_POINT, XSDK_FILTER_POINT, XSDK_FILTER_POINT, XSDK_ADDRESS_CLAMP,
		XSDK_ADDRESS_CLAMP, 0.0f, 0 };
	XsdkSamplerState sampler;
	xsdk_texture_sampler_state(&sampler, &point_clamped);
	volatile uint32_t *const vertices = xsdk_gpu_alloc(cell_count * QUAD_BYTES);
	if (vertices) {
		for (uint32_t i = 0; i < cell_count; i++)
			put_quad(vertices + i * QUAD_BYTES / 4, &cells[i]);
		__asm__ volatile("sync" ::: "memory");
		xsdk_gpu_invalidate(xsdk_gpu_address(vertices), cell_count * QUAD_BYTES);
	}
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("formats: vertex and front buffers", vertices && buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && made && vertices && buffers[0] && buffers[1]) {
		const Scene scene = { .vertex = &vertex,
			.pixel = &pixel,
			.textures = textures,
			.sampler = &sampler,
			.cells = cells,
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
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("formats: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
