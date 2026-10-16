#include <string.h>

#include "xsc_private.h"

// Shader containers, as xenos-toolchain/xdis/container_emit.py builds them (docs/container.md there).

enum {
	HEADER_WORDS = 9,
	HEADER_BYTES = 0x24,
	TABLE_HEADER_BYTES = 0x1C,
	CONSTANT_INFO_BYTES = 0x14,
	TYPE_INFO_BYTES = 0x10,
	PAD = 0xAB,
	TABLE_FLAGS = 0x1000,
	INTERPOLATOR_INFO = 0x2000 | 0x3,
	VERTEX_FIELD18 = 1,
	VERTEX_MYSTERY = 0x290,
	// XenosRecomp's mask of a pixel shader's outputs, oC0 alone unless the parts say (shader.h:80-92).
	PIXEL_OUTPUTS = 0x1,
	EXPORT_SLOT = 0x1000,
};

static const char creator[] = "xost";

typedef struct {
	Xsc *x;
	uint8_t *bytes;
	size_t length;
	size_t capacity;
} Bytes;

static bool put_bytes(Bytes *b, const void *data, size_t count)
{
	while (b->length + count > b->capacity) {
		if (!xsdk_xsc_room(b->x, &b->bytes, b->capacity, &b->capacity, 1))
			return false;
	}
	memcpy(b->bytes + b->length, data, count);
	b->length += count;
	return true;
}

static bool put_word(Bytes *b, uint32_t word)
{
	const uint8_t big[4] = { (uint8_t)(word >> 24), (uint8_t)(word >> 16), (uint8_t)(word >> 8), (uint8_t)word };
	return put_bytes(b, big, sizeof big);
}

static bool put_half(Bytes *b, uint32_t half)
{
	const uint8_t big[2] = { (uint8_t)(half >> 8), (uint8_t)half };
	return put_bytes(b, big, sizeof big);
}

static void set_word(Bytes *b, size_t at, uint32_t word)
{
	b->bytes[at] = (uint8_t)(word >> 24);
	b->bytes[at + 1] = (uint8_t)(word >> 16);
	b->bytes[at + 2] = (uint8_t)(word >> 8);
	b->bytes[at + 3] = (uint8_t)word;
}

static void set_half(Bytes *b, size_t at, uint32_t half)
{
	b->bytes[at] = (uint8_t)(half >> 8);
	b->bytes[at + 1] = (uint8_t)half;
}

static bool pad4(Bytes *b)
{
	const uint8_t pad = PAD;
	while (b->length % 4) {
		if (!put_bytes(b, &pad, 1))
			return false;
	}
	return true;
}

// The D3DX-style constant table, its size word first (container_emit.py build_constant_table).
static bool constant_table(Xsc *x, const XscContainerParts *parts, Bytes *body)
{
	const size_t count = parts->constant_count;
	size_t *const name_off = xsdk_xsc_alloc(x, (count + 1) * sizeof *name_off);
	size_t *const type_off = xsdk_xsc_alloc(x, (count + 1) * sizeof *type_off);
	if (!name_off || !type_off)
		return false;
	const uint8_t zero = 0;
	for (size_t i = 0; i < TABLE_HEADER_BYTES + CONSTANT_INFO_BYTES * count; i++) {
		if (!put_bytes(body, &zero, 1))
			return false;
	}
	for (size_t i = 0; i < count; i++) {
		const XscConstant *const c = &parts->constants[i];
		name_off[i] = body->length;
		if (!put_bytes(body, c->name, strlen(c->name) + 1) || !pad4(body))
			return false;
		type_off[i] = body->length;
		if (!put_half(body, c->param_class) || !put_half(body, c->param_type) || !put_half(body, c->rows) ||
			!put_half(body, c->columns) || !put_half(body, 1) || !put_half(body, 0) || !put_word(body, 0))
			return false;
	}
	const size_t target_off = body->length;
	const char *const target = parts->pixel ? "ps_3_0" : "vs_3_0";
	if (!put_bytes(body, target, strlen(target) + 1))
		return false;
	const size_t creator_off = body->length;
	if (!put_bytes(body, creator, sizeof creator) || !pad4(body))
		return false;
	const uint32_t header[7] = { TABLE_HEADER_BYTES, (uint32_t)creator_off, parts->pixel ? 0xFFFF0300 : 0xFFFE0300,
		(uint32_t)count, count ? TABLE_HEADER_BYTES : 0, TABLE_FLAGS, (uint32_t)target_off };
	for (int i = 0; i < 7; i++)
		set_word(body, 4 * (size_t)i, header[i]);
	for (size_t i = 0; i < count; i++) {
		const XscConstant *const c = &parts->constants[i];
		// Constants of one name point at the last one's strings, as a dict keyed by name keeps them.
		size_t last = i;
		for (size_t j = i + 1; j < count; j++) {
			if (!strcmp(parts->constants[j].name, c->name))
				last = j;
		}
		const size_t at = TABLE_HEADER_BYTES + i * CONSTANT_INFO_BYTES;
		set_word(body, at, (uint32_t)name_off[last]);
		set_half(body, at + 4, c->register_set);
		set_half(body, at + 6, c->register_index);
		set_half(body, at + 8, c->register_count);
		set_half(body, at + 10, 0);
		set_word(body, at + 12, (uint32_t)type_off[last]);
		set_word(body, at + 16, 0);
	}
	return true;
}

static bool shader_record(const XscContainerParts *parts, Bytes *rec)
{
	const uint32_t interpolator_info = INTERPOLATOR_INFO | (uint32_t)parts->interpolator_count << 5;
	const uint32_t head[6] = { 0, (uint32_t)parts->ucode_bytes, parts->program_control, parts->context_misc,
		parts->field10, interpolator_info };
	for (int i = 0; i < 6; i++) {
		if (!put_word(rec, head[i]))
			return false;
	}
	if (parts->pixel) {
		if (!put_word(rec, parts->ps_field18) ||
			!put_word(rec, parts->pixel_outputs ? parts->pixel_outputs : PIXEL_OUTPUTS))
			return false;
	} else {
		if (!put_word(rec, VERTEX_FIELD18) || !put_word(rec, (uint32_t)parts->vertex_element_count) ||
			!put_word(rec, (uint32_t)parts->interpolator_count) || !put_word(rec, VERTEX_MYSTERY))
			return false;
		for (size_t i = 0; i < parts->vertex_element_count; i++) {
			const XscVertexElement *const e = &parts->vertex_elements[i];
			if (!put_word(rec, e->flags << 20 | e->usage_index << 16 | e->usage << 12 | e->slot))
				return false;
		}
	}
	for (size_t i = 0; i < parts->interpolator_count; i++) {
		const XscInterpolator *const it = &parts->interpolators[i];
		if (!put_word(rec, it->flags << 12 | it->reg << 8 | it->usage << 4 | it->usage_index))
			return false;
	}
	for (size_t i = 0; !parts->pixel && i < parts->export_slot_count; i++) {
		if (!put_word(rec, EXPORT_SLOT | (uint32_t)parts->export_slots[i]))
			return false;
	}
	return true;
}

bool xsdk_xsc_container(Xsc *x, const XscContainerParts *parts, uint8_t **out, size_t *out_bytes)
{
	Bytes body = { .x = x };
	Bytes rec = { .x = x };
	if (!constant_table(x, parts, &body) || !shader_record(parts, &rec))
		return false;
	const size_t table_bytes = 4 + body.length;
	const size_t shader_off = HEADER_BYTES + table_bytes;
	const size_t virtual_size = shader_off + rec.length;
	Bytes all = { .x = x };
	const uint32_t header[HEADER_WORDS] = { parts->pixel ? 0x102A1100 : 0x102A1101, (uint32_t)virtual_size,
		(uint32_t)parts->ucode_bytes, 0, HEADER_BYTES, 0, (uint32_t)shader_off, 0, 0 };
	for (int i = 0; i < HEADER_WORDS; i++) {
		if (!put_word(&all, header[i]))
			return false;
	}
	if (!put_word(&all, (uint32_t)body.length) || !put_bytes(&all, body.bytes, body.length) ||
		!put_bytes(&all, rec.bytes, rec.length) || !put_bytes(&all, parts->ucode, parts->ucode_bytes))
		return false;
	*out = all.bytes;
	*out_bytes = all.length;
	return true;
}
