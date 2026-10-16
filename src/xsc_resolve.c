#include <stdio.h>
#include <string.h>

#include "xsc_private.h"

// Types and the shader interface, as xenos-toolchain/xsc/resolve.py resolves them.

enum {
	SC_UNIFORM_CONSTANT = 0,
	SC_INPUT = 1,
	SC_UNIFORM = 2,
	SC_OUTPUT = 3,
	SC_PUSH_CONSTANT = 9,
	BUILTIN_POSITION = 0,
	BUILTIN_VERTEX_ID = 5,
	BUILTIN_INSTANCE_ID = 6,
	BUILTIN_FRAG_COORD = 15,
	BUILTIN_FRONT_FACING = 17,
	BUILTIN_FRAG_DEPTH = 22,
	// The console loads float constants from memory in whole groups of 4 (docs/HARDWARE.md, "Constants loaded from
	// memory go in groups of 4 too").
	CONSTANT_GROUP = 4,
	DEFAULT_STRIDE = 16,
	NAME_BYTES = 32,
};

static const char *const type_names[] = { "none", "void", "bool", "int", "float", "vector", "matrix", "image",
	"sampler", "sampledimage", "array", "struct", "pointer", "function" };

const char *xsdk_xsc_type_kind_name(XscTypeKind kind)
{
	return type_names[kind];
}

XscType *xsdk_xsc_resolve_types(Xsc *x, const XscModule *m)
{
	XscType *const types = xsdk_xsc_alloc(x, (size_t)m->id_limit * sizeof *types);
	if (!types)
		return NULL;
	for (uint32_t id = 0; id < m->id_limit; id++)
		types[id].dim = XSC_NONE;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		uint32_t count;
		const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
		XscType t = { .dim = XSC_NONE };
		switch (ins->opcode) {
		case SPV_TYPE_VOID:
			t.kind = XSC_TYPE_VOID;
			break;
		case SPV_TYPE_BOOL:
			t.kind = XSC_TYPE_BOOL;
			break;
		case SPV_TYPE_INT:
			t.kind = XSC_TYPE_INT;
			break;
		case SPV_TYPE_FLOAT:
			t.kind = XSC_TYPE_FLOAT;
			break;
		case SPV_TYPE_VECTOR:
		case SPV_TYPE_MATRIX:
		case SPV_TYPE_ARRAY:
			t.kind = ins->opcode == SPV_TYPE_VECTOR ? XSC_TYPE_VECTOR
				: ins->opcode == SPV_TYPE_MATRIX    ? XSC_TYPE_MATRIX
													: XSC_TYPE_ARRAY;
			t.comp = xsdk_xsc_operand(ins, 0);
			t.count = xsdk_xsc_operand(ins, 1);
			break;
		case SPV_TYPE_IMAGE:
			t.kind = XSC_TYPE_IMAGE;
			t.comp = xsdk_xsc_operand(ins, 0);
			t.dim = xsdk_xsc_operand(ins, 1);
			t.depth = xsdk_xsc_operand(ins, 2) == 1;
			break;
		case SPV_TYPE_SAMPLER:
			t.kind = XSC_TYPE_SAMPLER;
			break;
		case SPV_TYPE_SAMPLED_IMAGE:
			t.kind = XSC_TYPE_SAMPLED_IMAGE;
			t.comp = xsdk_xsc_operand(ins, 0);
			break;
		case SPV_TYPE_STRUCT:
			t.kind = XSC_TYPE_STRUCT;
			t.members = ops;
			t.member_count = count;
			break;
		case SPV_TYPE_POINTER:
			t.kind = XSC_TYPE_POINTER;
			t.storage_class = xsdk_xsc_operand(ins, 0);
			t.comp = xsdk_xsc_operand(ins, 1);
			break;
		case SPV_TYPE_FUNCTION:
			t.kind = XSC_TYPE_FUNCTION;
			break;
		default:
			continue;
		}
		if (ins->result_id < m->id_limit)
			types[ins->result_id] = t;
	}
	return types;
}

const XscType *xsdk_xsc_type(const XscModule *m, const XscType *types, uint64_t id)
{
	if (id >= m->id_limit || types[id].kind == XSC_TYPE_NONE)
		return NULL;
	return &types[id];
}

// A member's decoration of a kind, its first argument or fallback; -1 for a decoration with none.
static int64_t member_arg(const XscModule *m, uint32_t target, uint32_t member, uint32_t decoration, int64_t fallback)
{
	return xsdk_xsc_decoration_arg(xsdk_xsc_member_decoration(m, target, member, decoration), fallback);
}

// Bytes a block member of this type spans, by its std140 decorations; -1, with the error set, for one it can't size.
static int64_t type_bytes(
	Xsc *x, const XscModule *m, const XscType *types, uint64_t tid, int64_t member_struct, int64_t member_index)
{
	const XscType *const t = xsdk_xsc_type(m, types, tid);
	if (!t || t->kind == XSC_TYPE_FLOAT || t->kind == XSC_TYPE_INT || t->kind == XSC_TYPE_BOOL)
		return 4;
	if (t->kind == XSC_TYPE_VECTOR)
		return 4 * (int64_t)t->count;
	if (t->kind == XSC_TYPE_MATRIX) {
		const bool member = member_struct != XSC_NONE;
		const int64_t stride = member
			? member_arg(m, (uint32_t)member_struct, (uint32_t)member_index, SPV_DEC_MATRIX_STRIDE, DEFAULT_STRIDE)
			: DEFAULT_STRIDE;
		const XscType *const col = xsdk_xsc_type(m, types, t->comp);
		const int64_t rows = col && col->kind == XSC_TYPE_VECTOR ? col->count : 1;
		const bool row_major =
			member && xsdk_xsc_member_decoration(m, (uint32_t)member_struct, (uint32_t)member_index, SPV_DEC_ROW_MAJOR);
		return stride * (row_major ? rows : t->count);
	}
	if (t->kind == XSC_TYPE_ARRAY) {
		const int64_t stride =
			xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, (uint32_t)tid, SPV_DEC_ARRAY_STRIDE), DEFAULT_STRIDE);
		const XscInstr *const lc = xsdk_xsc_by_result(m, t->count);
		const int64_t length = lc && lc->opcode == SPV_CONSTANT ? xsdk_xsc_operand(lc, 0) : 1;
		return stride * length;
	}
	if (t->kind == XSC_TYPE_STRUCT) {
		int64_t most = 0;
		for (uint32_t i = 0; i < t->member_count; i++) {
			const int64_t offset = member_arg(m, (uint32_t)tid, i, SPV_DEC_OFFSET, 0);
			const int64_t bytes = type_bytes(x, m, types, t->members[i], (int64_t)tid, i);
			if (bytes < 0)
				return -1;
			if (i == 0 || offset + bytes > most)
				most = offset + bytes;
		}
		return most;
	}
	xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "block member of type %s", xsdk_xsc_type_kind_name(t->kind));
	return -1;
}

static bool set_role(Xsc *x, XscInterface *si, uint32_t id, XscRole role)
{
	if (id >= si->role_limit)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "variable %%%u past the module's ids", (unsigned)id);
	si->roles[id] = role;
	return true;
}

static bool add_use(Xsc *x, XscVarUse **list, size_t *count, size_t *capacity, XscVarUse use)
{
	if (!xsdk_xsc_room(x, list, *count, capacity, sizeof **list))
		return false;
	(*list)[(*count)++] = use;
	return true;
}

static bool add_constant(Xsc *x, XscInterface *si, size_t *capacity, XscConstantSlot slot)
{
	if (!xsdk_xsc_room(x, &si->constants, si->constant_count, capacity, sizeof *si->constants))
		return false;
	si->constants[si->constant_count++] = slot;
	return true;
}

static bool add_sampler_dim(Xsc *x, XscInterface *si, size_t *capacity, int64_t slot, int64_t dim)
{
	if (!xsdk_xsc_room(x, &si->sampler_dims, si->sampler_dim_count, capacity, 2 * sizeof *si->sampler_dims))
		return false;
	// Two entries per pair: grown in pairs, counted in pairs.
	si->sampler_dims[2 * si->sampler_dim_count] = slot;
	si->sampler_dims[2 * si->sampler_dim_count + 1] = dim;
	si->sampler_dim_count++;
	return true;
}

bool xsdk_xsc_sampler_dim(const XscInterface *si, uint32_t slot, int64_t *dim)
{
	bool found = false;
	for (size_t i = 0; i < si->sampler_dim_count; i++) {
		if (si->sampler_dims[2 * i] == slot) {
			*dim = si->sampler_dims[2 * i + 1];
			found = true;
		}
	}
	return found;
}

const XscRole *xsdk_xsc_role(const XscInterface *si, uint32_t id)
{
	static const XscRole none = { XSC_ROLE_NONE, 0, 0, 0 };
	return id < si->role_limit ? &si->roles[id] : &none;
}

static uint32_t usage_for_vector(const XscModule *m, const XscType *types, int64_t pointee)
{
	const XscType *const shape = pointee == XSC_NONE ? NULL : xsdk_xsc_type(m, types, (uint64_t)pointee);
	return shape && shape->kind == XSC_TYPE_VECTOR && shape->count == 2 ? XSC_USAGE_TEXCOORD : XSC_USAGE_COLOR;
}

static uint32_t location_or_zero(const XscVar *v)
{
	return v->location == XSC_NONE ? 0 : (uint32_t)v->location;
}

typedef struct {
	size_t vfetch;
	size_t interp_out;
	size_t interp_in;
	size_t color_out;
	size_t samplers;
	size_t constants;
	size_t blocks;
	size_t sampler_dims;
	size_t pending;
	size_t vulkan_samplers;
	size_t comparisons;
} ListCapacities;

typedef struct {
	bool push;
	const XscVar *var;
} PendingBlock;

static int64_t round_up(int64_t reg, int64_t group)
{
	return (reg + group - 1) / group * group;
}

static bool block_before(const PendingBlock *a, const PendingBlock *b)
{
	const int64_t a_key[3] = { !a->push, a->var->descriptor_set == XSC_NONE ? 0 : a->var->descriptor_set,
		a->var->binding == XSC_NONE ? 0 : a->var->binding };
	const int64_t b_key[3] = { !b->push, b->var->descriptor_set == XSC_NONE ? 0 : b->var->descriptor_set,
		b->var->binding == XSC_NONE ? 0 : b->var->binding };
	for (int i = 0; i < 3; i++) {
		if (a_key[i] != b_key[i])
			return a_key[i] < b_key[i];
	}
	return false;
}

// Registers for each block, push constants first, then (set, binding); each uniform block has whole groups of 4 to
// itself (resolve.py _assign_blocks).
static bool assign_blocks(Xsc *x, const XscModule *m, const XscType *types, XscInterface *si, PendingBlock *pending,
	size_t pending_count, int64_t first_reg, ListCapacities *capacity, int64_t *end)
{
	// Python's sort is stable: an insertion sort keeps equal keys in their order.
	for (size_t i = 1; i < pending_count; i++) {
		const PendingBlock moving = pending[i];
		size_t j = i;
		for (; j > 0 && block_before(&moving, &pending[j - 1]); j--)
			pending[j] = pending[j - 1];
		pending[j] = moving;
	}
	int64_t reg = first_reg;
	for (size_t b = 0; b < pending_count; b++) {
		const XscVar *const v = pending[b].var;
		if (!pending[b].push)
			reg = round_up(reg, CONSTANT_GROUP);
		const int64_t bytes = type_bytes(x, m, types, (uint64_t)v->pointee, XSC_NONE, 0);
		if (bytes < 0)
			return false;
		const int64_t regs = (bytes + 15) / 16;
		if (!set_role(x, si, v->vid, (XscRole){ XSC_ROLE_BLOCK, (int64_t)si->block_count, 0, 0 }))
			return false;
		if (!xsdk_xsc_room(x, &si->blocks, si->block_count, &capacity->blocks, sizeof *si->blocks))
			return false;
		si->blocks[si->block_count++] = (XscBlock){ .push = pending[b].push,
			.var = v,
			.struct_type = (uint32_t)v->pointee,
			.reg = (uint32_t)reg,
			.regs = (uint32_t)regs };
		const XscType *const st = xsdk_xsc_type(m, types, (uint64_t)v->pointee);
		for (uint32_t mi = 0; st && mi < st->member_count; mi++) {
			const int64_t off = member_arg(m, (uint32_t)v->pointee, mi, SPV_DEC_OFFSET, 16 * (int64_t)mi);
			const int64_t member_bytes = type_bytes(x, m, types, st->members[mi], v->pointee, mi);
			if (member_bytes < 0)
				return false;
			const int64_t mregs = (member_bytes + off % 16 + 15) / 16;
			const char *name = xsdk_xsc_member_name(m, (uint32_t)v->pointee, mi);
			if (!name) {
				char text[NAME_BYTES];
				const int length = snprintf(text, sizeof text, "m%u", (unsigned)mi);
				name = xsdk_xsc_copy_text(x, text, (size_t)length);
				if (!name)
					return false;
			}
			if (!add_constant(x, si, &capacity->constants,
					(XscConstantSlot){ name, (uint32_t)(reg + off / 16), (uint32_t)mregs, (int32_t)mi }))
				return false;
		}
		reg += regs;
		if (!pending[b].push)
			reg = round_up(reg, CONSTANT_GROUP);
	}
	*end = reg;
	return true;
}

static bool read_vars(Xsc *x, const XscModule *m, const XscType *types, XscVar **vars, size_t *var_count)
{
	size_t capacity = 0;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode != SPV_VARIABLE)
			continue;
		const XscType *const ptr = xsdk_xsc_type(m, types, ins->result_type);
		const uint32_t id = ins->result_id;
		if (!xsdk_xsc_room(x, vars, *var_count, &capacity, sizeof **vars))
			return false;
		(*vars)[(*var_count)++] = (XscVar){ .vid = id,
			.name = xsdk_xsc_name(m, id, ""),
			.storage_class = xsdk_xsc_operand(ins, 0),
			.pointee = ptr ? (int64_t)ptr->comp : XSC_NONE,
			.location = xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, id, SPV_DEC_LOCATION), XSC_NONE),
			.binding = xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, id, SPV_DEC_BINDING), XSC_NONE),
			.builtin = xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, id, SPV_DEC_BUILTIN), XSC_NONE),
			.descriptor_set = xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, id, SPV_DEC_DESCRIPTOR_SET), XSC_NONE) };
	}
	return true;
}

static bool resolve_input(
	Xsc *x, const XscModule *m, const XscType *types, XscInterface *si, const XscVar *v, ListCapacities *capacity)
{
	if (v->builtin == BUILTIN_VERTEX_ID || v->builtin == BUILTIN_INSTANCE_ID)
		return true;
	if (!si->pixel) {
		const uint32_t loc = location_or_zero(v);
		const uint32_t usage = loc == 0 ? XSC_USAGE_POSITION : loc == 1 ? XSC_USAGE_COLOR : XSC_USAGE_TEXCOORD;
		const uint32_t usage_index = loc > 2 ? loc - 2 : 0;
		if (!add_use(x, &si->vfetch, &si->vfetch_count, &capacity->vfetch, (XscVarUse){ v, usage, usage_index }))
			return false;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_VFETCH, (int64_t)si->vfetch_count - 1, 0, 0 });
	}
	if (v->builtin == BUILTIN_FRAG_COORD) {
		si->frag_coord = v;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_FRAG_COORD, 0, 0, 0 });
	}
	if (v->builtin == BUILTIN_FRONT_FACING) {
		si->front_facing = v;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_FRONT_FACING, 0, 0, 0 });
	}
	const uint32_t r = location_or_zero(v);
	if (!add_use(x, &si->interp_in, &si->interp_in_count, &capacity->interp_in,
			(XscVarUse){ v, r, usage_for_vector(m, types, v->pointee) }))
		return false;
	return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_INTERP_IN, r, 0, 0 });
}

static bool resolve_output(
	Xsc *x, const XscModule *m, const XscType *types, XscInterface *si, const XscVar *v, ListCapacities *capacity)
{
	const XscType *const st = v->pointee == XSC_NONE ? NULL : xsdk_xsc_type(m, types, (uint64_t)v->pointee);
	if (st && st->kind == XSC_TYPE_STRUCT) {
		// gl_PerVertex: the member decorated BuiltIn Position is gl_Position.
		for (uint32_t mi = 0; mi < st->member_count; mi++) {
			for (size_t i = 0; i < m->member_decoration_count; i++) {
				const XscDecoration *const d = &m->member_decorations[i];
				if (d->target != (uint32_t)v->pointee || d->member != mi || d->decoration != SPV_DEC_BUILTIN ||
					!d->arg_count || d->args[0] != BUILTIN_POSITION)
					continue;
				si->position_out = v->vid;
				if (!set_role(x, si, v->vid, (XscRole){ XSC_ROLE_POSITION_STRUCT, mi, 0, 0 }))
					return false;
			}
		}
		return true;
	}
	if (v->builtin == BUILTIN_POSITION) {
		si->position_out = v->vid;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_POSITION_STRUCT, XSC_NONE, 0, 0 });
	}
	if (v->builtin == BUILTIN_FRAG_DEPTH && si->pixel)
		si->frag_depth = v;
	if (v->builtin != XSC_NONE)
		return true;
	const uint32_t loc = location_or_zero(v);
	if (!si->pixel) {
		if (!add_use(x, &si->interp_out, &si->interp_out_count, &capacity->interp_out,
				(XscVarUse){ v, loc, usage_for_vector(m, types, v->pointee) }))
			return false;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_INTERP_OUT, loc, 0, 0 });
	}
	if (!add_use(x, &si->color_out, &si->color_out_count, &capacity->color_out, (XscVarUse){ v, loc, 0 }))
		return false;
	return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_COLOR, loc, 0, 0 });
}

static const XscType *image_of(const XscModule *m, const XscType *types, const XscType *st)
{
	const XscType *const img = st->kind == XSC_TYPE_SAMPLED_IMAGE ? xsdk_xsc_type(m, types, st->comp) : st;
	return img && img->kind == XSC_TYPE_IMAGE ? img : NULL;
}

static int64_t image_dim(const XscModule *m, const XscType *types, const XscType *st)
{
	const XscType *const img = image_of(m, types, st);
	return img ? img->dim : XSC_NONE;
}

static bool resolve_uniform_constant(Xsc *x, const XscModule *m, const XscType *types, XscInterface *si,
	const XscVar *v, int64_t *next_const, const XscVar ***vulkan_samplers, size_t *vulkan_count,
	ListCapacities *capacity)
{
	const XscType *const st = v->pointee == XSC_NONE ? NULL : xsdk_xsc_type(m, types, (uint64_t)v->pointee);
	if (!st)
		return true;
	const bool image = st->kind == XSC_TYPE_SAMPLED_IMAGE || st->kind == XSC_TYPE_IMAGE || st->kind == XSC_TYPE_SAMPLER;
	if (image && v->descriptor_set != XSC_NONE) {
		if (!xsdk_xsc_room(x, vulkan_samplers, *vulkan_count, &capacity->vulkan_samplers, sizeof **vulkan_samplers))
			return false;
		(*vulkan_samplers)[(*vulkan_count)++] = v;
		return true;
	}
	if (image) {
		const int64_t slot = v->binding != XSC_NONE ? v->binding : (int64_t)si->sampler_count;
		const int64_t dim = image_dim(m, types, st);
		if (!add_use(x, &si->samplers, &si->sampler_count, &capacity->samplers, (XscVarUse){ v, (uint32_t)slot, 0 }))
			return false;
		if (!add_sampler_dim(x, si, &capacity->sampler_dims, slot, dim))
			return false;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_SAMPLER, slot, dim, XSC_NONE });
	}
	if (st->kind != XSC_TYPE_MATRIX && st->kind != XSC_TYPE_VECTOR && st->kind != XSC_TYPE_FLOAT &&
		st->kind != XSC_TYPE_ARRAY)
		return true;
	int64_t regs = st->kind == XSC_TYPE_MATRIX ? st->count : 1;
	int64_t elem_regs = 1;
	if (st->kind == XSC_TYPE_ARRAY) {
		// A uniform array: N contiguous registers, element i at base + i.
		const XscType *const elem = xsdk_xsc_type(m, types, st->comp);
		elem_regs = elem && elem->kind == XSC_TYPE_MATRIX ? elem->count : 1;
		const XscInstr *const lc = xsdk_xsc_by_result(m, st->count);
		const int64_t length = lc && lc->opcode == SPV_CONSTANT ? xsdk_xsc_operand(lc, 0) : 1;
		regs = (length > 1 ? length : 1) * elem_regs;
	}
	const int64_t reg = v->location != XSC_NONE ? v->location : *next_const;
	if (reg + regs > *next_const)
		*next_const = reg + regs;
	const char *name = v->name;
	if (!name[0]) {
		char text[NAME_BYTES];
		const int length = snprintf(text, sizeof text, "u%lld", (long long)reg);
		name = xsdk_xsc_copy_text(x, text, (size_t)length);
		if (!name)
			return false;
	}
	if (!add_constant(x, si, &capacity->constants, (XscConstantSlot){ name, (uint32_t)reg, (uint32_t)regs, -1 }))
		return false;
	if (st->kind == XSC_TYPE_ARRAY)
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_ARRAY, reg, regs, elem_regs });
	return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_CONSTANT, reg, regs, 0 });
}

static void sort_uses(XscVarUse *uses, size_t count, bool by_location)
{
	for (size_t i = 1; i < count; i++) {
		const XscVarUse moving = uses[i];
		const int64_t key = by_location ? location_or_zero(moving.var) : moving.a;
		size_t j = i;
		for (; j > 0; j--) {
			const int64_t before = by_location ? location_or_zero(uses[j - 1].var) : uses[j - 1].a;
			if (before <= key)
				break;
			uses[j] = uses[j - 1];
		}
		uses[j] = moving;
	}
}

static bool vulkan_sampler_before(const XscVar *a, const XscVar *b)
{
	if (a->descriptor_set != b->descriptor_set)
		return a->descriptor_set < b->descriptor_set;
	return (a->binding == XSC_NONE ? 0 : a->binding) < (b->binding == XSC_NONE ? 0 : b->binding);
}

static bool add_vulkan_samplers(Xsc *x, const XscModule *m, const XscType *types, XscInterface *si,
	const XscVar **samplers, size_t count, ListCapacities *capacity)
{
	for (size_t i = 1; i < count; i++) {
		const XscVar *const moving = samplers[i];
		size_t j = i;
		for (; j > 0 && vulkan_sampler_before(moving, samplers[j - 1]); j--)
			samplers[j] = samplers[j - 1];
		samplers[j] = moving;
	}
	for (size_t slot = 0; slot < count; slot++) {
		const XscVar *const v = samplers[slot];
		const XscType *const st = xsdk_xsc_type(m, types, (uint64_t)v->pointee);
		const int64_t dim = image_dim(m, types, st);
		if (!add_use(x, &si->samplers, &si->sampler_count, &capacity->samplers, (XscVarUse){ v, (uint32_t)slot, 0 }))
			return false;
		if (!add_sampler_dim(x, si, &capacity->sampler_dims, (int64_t)slot, dim))
			return false;
		if (!set_role(x, si, v->vid, (XscRole){ XSC_ROLE_SAMPLER, (int64_t)slot, dim, XSC_NONE }))
			return false;
	}
	return true;
}

// A register past the blocks for each sampler of a depth image, holding its compare op's weights, which the runtime
// writes from the sampler it binds: the comparison runs after the fetch.
static bool assign_comparisons(
	Xsc *x, const XscModule *m, const XscType *types, XscInterface *si, ListCapacities *capacity)
{
	for (size_t i = 0; i < si->sampler_count; i++) {
		const XscVarUse *const use = &si->samplers[i];
		const XscType *const st = xsdk_xsc_type(m, types, (uint64_t)use->var->pointee);
		const XscType *const img = st ? image_of(m, types, st) : NULL;
		if (!img || !img->depth)
			continue;
		const uint32_t reg = si->constant_end++;
		char text[NAME_BYTES];
		const int length = snprintf(text, sizeof text, "_compare%u", (unsigned)use->a);
		const char *const name = xsdk_xsc_copy_text(x, text, (size_t)length);
		if (!name || !add_constant(x, si, &capacity->constants, (XscConstantSlot){ name, reg, 1, -1 }) ||
			!add_use(x, &si->comparisons, &si->comparison_count, &capacity->comparisons,
				(XscVarUse){ use->var, reg, use->a }))
			return false;
		si->roles[use->var->vid].c = reg;
	}
	return true;
}

enum {
	// Vulkan's gl_InstanceIndex, firstInstance included.
	BUILTIN_INSTANCE_INDEX = 43,
};

// gl_InstanceIndex in a vertex shader takes the register past the blocks, so it shares no group of 4 with a uniform
// block the GPU may load, as a group loaded mustn't also be set (include/xsdk/gpu.h, xsdk_gpu_load_vertex_constants).
static bool assign_instance_index(Xsc *x, XscInterface *si, const XscVar *vars, size_t var_count, int64_t *end)
{
	for (size_t i = 0; i < var_count && !si->pixel; i++) {
		if (vars[i].storage_class != SC_INPUT || vars[i].builtin != BUILTIN_INSTANCE_INDEX)
			continue;
		if (!set_role(x, si, vars[i].vid, (XscRole){ XSC_ROLE_INSTANCE_INDEX, *end, 0, 0 }))
			return false;
		(*end)++;
	}
	return true;
}

int64_t xsdk_xsc_instance_index_reg(const XscInterface *si)
{
	for (uint32_t id = 0; id < si->role_limit; id++) {
		if (si->roles[id].kind == XSC_ROLE_INSTANCE_INDEX)
			return si->roles[id].a;
	}
	return XSC_NONE;
}
bool xsdk_xsc_resolve_interface(Xsc *x, const XscModule *m, XscInterface *si)
{
	memset(si, 0, sizeof *si);
	if (!m->entry_point_count)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "list index out of range");
	const XscType *const types = xsdk_xsc_resolve_types(x, m);
	if (!types)
		return false;
	si->pixel = m->entry_points[0].model != 0;
	si->role_limit = m->id_limit;
	si->roles = xsdk_xsc_alloc(x, (size_t)m->id_limit * sizeof *si->roles);
	if (!si->roles)
		return false;
	XscVar *vars = NULL;
	size_t var_count = 0;
	if (!read_vars(x, m, types, &vars, &var_count))
		return false;
	ListCapacities capacity = { 0 };
	PendingBlock *pending = NULL;
	size_t pending_count = 0;
	const XscVar **vulkan_samplers = NULL;
	size_t vulkan_count = 0;
	int64_t next_const = 0;
	for (size_t i = 0; i < var_count; i++) {
		const XscVar *const v = &vars[i];
		bool ok = true;
		// A vertex shader's built-in isn't a stream: gl_InstanceIndex takes a register past the blocks, and loading any
		// other is refused.
		if (v->storage_class == SC_INPUT && (si->pixel || v->builtin == XSC_NONE)) {
			ok = resolve_input(x, m, types, si, v, &capacity);
		} else if (v->storage_class == SC_OUTPUT) {
			ok = resolve_output(x, m, types, si, v, &capacity);
		} else if (v->storage_class == SC_UNIFORM || v->storage_class == SC_PUSH_CONSTANT) {
			const XscType *const st = v->pointee == XSC_NONE ? NULL : xsdk_xsc_type(m, types, (uint64_t)v->pointee);
			if (st && st->kind == XSC_TYPE_STRUCT) {
				ok = xsdk_xsc_room(x, &pending, pending_count, &capacity.pending, sizeof *pending);
				if (ok)
					pending[pending_count++] = (PendingBlock){ v->storage_class == SC_PUSH_CONSTANT, v };
			}
		} else if (v->storage_class == SC_UNIFORM_CONSTANT) {
			ok = resolve_uniform_constant(x, m, types, si, v, &next_const, &vulkan_samplers, &vulkan_count, &capacity);
		}
		if (!ok)
			return false;
	}
	int64_t end;
	if (!assign_blocks(x, m, types, si, pending, pending_count, next_const, &capacity, &end) ||
		!assign_instance_index(x, si, vars, var_count, &end))
		return false;
	si->constant_end = (uint32_t)end;
	if (!add_vulkan_samplers(x, m, types, si, vulkan_samplers, vulkan_count, &capacity) ||
		!assign_comparisons(x, m, types, si, &capacity))
		return false;
	sort_uses(si->vfetch, si->vfetch_count, true);
	sort_uses(si->interp_in, si->interp_in_count, false);
	sort_uses(si->interp_out, si->interp_out_count, false);
	return true;
}
