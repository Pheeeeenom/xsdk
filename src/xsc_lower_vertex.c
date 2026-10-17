#include "xsc_lower.h"

// A vertex shader, lowered as lower.py's lower_vertex lowers it.

enum {
	// The rows and columns a matrix constant declares.
	MATRIX_SIZE = 4,
	// A texture fetch names one of 32 fetch constants (xenia:src/xenia/gpu/ucode.h:789-790).
	FETCH_SLOTS = 32,
	// The parameter type xost's containers give a sampler.
	SAMPLER_PARAMETER = 0xC,
	// gl_PerVertex's members by their BuiltIn decoration, and outputs' storage class (SPIR-V 1.0's enums).
	BUILTIN_CLIP_DISTANCE = 3,
	BUILTIN_CULL_DISTANCE = 4,
	STORAGE_OUTPUT = 3,
	// Xenos clips against six planes (xenia:src/xenia/gpu/registers.h:539-544).
	CLIP_PLANES = 6,
};

// Where gl_PerVertex's members are, XSC_NONE for those the block lacks or a gl_Position of its own.
typedef struct {
	int64_t position;
	int64_t clip_distance;
	int64_t cull_distance;
} PerVertex;

// The unroller merges a vertex shader's if/else arms (xsc_unroll.c); a branch it left would lower as its last arm
// alone, silently.
static bool has_branches(const XscModule *m)
{
	for (size_t i = 0; i < m->instr_count; i++) {
		if (m->instrs[i].opcode == SPV_BRANCH_CONDITIONAL || m->instrs[i].opcode == SPV_SWITCH)
			return true;
	}
	return false;
}

// Fetch r0, the vertex index, last (xenia:src/xenia/gpu/dxbc_shader_translator_fetch.cc:80-88).
static bool fetch_inputs(XscLower *l, const XscInterface *si)
{
	for (size_t n = si->vfetch_count; n-- > 0;) {
		const XscVarUse *const use = &si->vfetch[n];
		XscFetch f = xsdk_xsc_template_vfetch();
		f.dest = (int64_t)n;
		if (!xsdk_xsc_program_vfetch(l->x, l->p, use->a, use->b, &f))
			return false;
		l->inputs[use->var->vid] = (int64_t)n;
	}
	return true;
}

// The member of gl_PerVertex decorated with this BuiltIn.
static int64_t builtin_member(const XscModule *m, uint32_t block_type, int64_t builtin)
{
	for (size_t i = 0; i < m->member_decoration_count; i++) {
		const XscDecoration *const d = &m->member_decorations[i];
		if (d->target == block_type && d->decoration == SPV_DEC_BUILTIN && d->arg_count && d->args[0] == builtin)
			return d->member;
	}
	return XSC_NONE;
}

static PerVertex per_vertex(const XscModule *m, const XscType *types, const XscInterface *si)
{
	PerVertex pv = { xsdk_xsc_role(si, si->position_out)->a, XSC_NONE, XSC_NONE };
	const XscInstr *const var = xsdk_xsc_by_result(m, si->position_out);
	const XscType *const pointer = var ? xsdk_xsc_type(m, types, var->result_type) : NULL;
	if (pv.position == XSC_NONE || !pointer)
		return pv;
	pv.clip_distance = builtin_member(m, pointer->comp, BUILTIN_CLIP_DISTANCE);
	pv.cull_distance = builtin_member(m, pointer->comp, BUILTIN_CULL_DISTANCE);
	return pv;
}

// The gl_PerVertex member a pointer reaches, XSC_NONE for any other pointer.
static int64_t member_of(XscLower *l, uint32_t pointer)
{
	const XscInstr *const chain = xsdk_xsc_by_result(l->m, pointer);
	int64_t member;
	if (!chain || chain->opcode != SPV_ACCESS_CHAIN || xsdk_xsc_operand(chain, 0) != l->si->position_out ||
		!xsdk_xsc_int_const(l, xsdk_xsc_operand(chain, 1), &member))
		return XSC_NONE;
	return member;
}

// Whether a pointer is gl_Position's: a gl_PerVertex's member, or a gl_Position of its own.
static bool is_position_pointer(XscLower *l, const PerVertex *pv, uint32_t pointer)
{
	return pv->position == XSC_NONE ? pointer == l->si->position_out : member_of(l, pointer) == pv->position;
}

// gl_Position's value: its member's store alone, as the block's other members are stored through it too.
static uint32_t position_value(XscLower *l, const PerVertex *pv, const XscStores *stores)
{
	uint32_t value = 0;
	for (size_t i = 0; i < stores->target_count; i++) {
		if (is_position_pointer(l, pv, stores->targets[i]))
			value = stores->value[stores->targets[i]];
	}
	return value;
}

static bool export_interpolators(XscLower *l, const XscInterface *si, const XscStores *stores)
{
	for (size_t i = 0; i < si->interp_out_count; i++) {
		const XscVarUse *const use = &si->interp_out[i];
		const uint32_t value = stores->value[use->var->vid];
		if (!value)
			return xsdk_xsc_lower_refuse(l, "interpolator %s never written", use->var->name);
		const XscOperand r = xsdk_xsc_lower_operand(l, value);
		if (xsc_failed(l))
			return false;
		if (r.kind == XSC_OPERAND_MAT)
			return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "interpolator %s holds a matrix", use->var->name);
		XscAlu f = xsdk_xsc_template_mov();
		for (int n = 0; n < 2; n++) {
			f.src_reg[n] = r.value;
			f.src_is_temp[n] = r.kind == XSC_OPERAND_REG;
		}
		if (!xsdk_xsc_program_export(l->x, l->p, use->a, use->b, 0, &f))
			return false;
	}
	return true;
}

static bool is_sample(uint32_t opcode)
{
	return opcode == SPV_IMAGE_SAMPLE_IMPLICIT_LOD || opcode == SPV_IMAGE_SAMPLE_EXPLICIT_LOD ||
		opcode == SPV_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD || opcode == SPV_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD;
}

// Every sample, after the vertex fetches and before the exports, as a pixel shader gathers its samples first; the
// fetch constants they read marked.
static bool sample_all(XscLower *l, bool used[FETCH_SLOTS])
{
	for (size_t i = 0; i < l->m->instr_count; i++) {
		const XscInstr *const ins = &l->m->instrs[i];
		if (!is_sample(ins->opcode))
			continue;
		int64_t slot;
		if (!xsdk_xsc_emit_sample(l, ins->result_id, ins, &slot))
			return false;
		if (slot < 0 || slot >= FETCH_SLOTS)
			return xsdk_xsc_lower_refuse(l, "a sampler at fetch constant %lld, past 31", (long long)slot);
		used[slot] = true;
	}
	return true;
}

// The samplers first, as a pixel shader's container has them, then the uniforms and the literals.
static bool add_constants(XscLower *l, const XscInterface *si, const bool used[FETCH_SLOTS], XscConstants *constants)
{
	for (int64_t slot = 0; slot < FETCH_SLOTS; slot++) {
		const char *const name = used[slot] ? xsdk_xsc_format(l->x, "tex%lld", (long long)slot) : NULL;
		const XscConstant c = { name, XSC_REGISTER_SET_SAMPLER, (uint32_t)slot, 1, XSC_CLASS_OBJECT, SAMPLER_PARAMETER,
			1, 1 };
		if (used[slot] && (!name || !xsdk_xsc_add_constant(l->x, constants, c)))
			return false;
	}
	for (size_t i = 0; i < si->constant_count; i++) {
		const XscConstantSlot *const cs = &si->constants[i];
		XscConstant c = xsdk_xsc_vector_constant(cs->name, cs->reg, 1);
		if (cs->regs > 1)
			c = (XscConstant){ cs->name, XSC_REGISTER_SET_FLOAT4, cs->reg, cs->regs, XSC_CLASS_MATRIX_COLUMNS,
				XSC_TYPE_FLOAT_PARAMETER, MATRIX_SIZE, MATRIX_SIZE };
		if (!xsdk_xsc_add_constant(l->x, constants, c))
			return false;
	}
	const int64_t instance = xsdk_xsc_instance_index_reg(si);
	if (instance != XSC_NONE &&
		!xsdk_xsc_add_constant(l->x, constants, xsdk_xsc_vector_constant("gl_InstanceIndex", instance, 1)))
		return false;
	return xsdk_xsc_literal_constants(l, constants);
}

// The position, then the interpolators; without any, they're allocated first, as xsdk's resolve shader is.
static bool lower_body(XscLower *l, const XscInterface *si, const XscStores *stores, uint32_t position)
{
	if (!si->interp_out_count && !xsdk_xsc_program_alloc_interpolators(l->x, l->p))
		return false;
	if (!xsdk_xsc_program_alloc(l->x, l->p, XSC_ALLOC_POSITION))
		return false;
	if (!position)
		return xsdk_xsc_lower_refuse(l, "no write to gl_Position");
	if (!xsdk_xsc_emit_value(l, position, XSC_EXPORT_POSITION, true, true))
		return false;
	if (si->interp_out_count && !xsdk_xsc_program_alloc_interpolators(l->x, l->p))
		return false;
	return export_interpolators(l, si, stores);
}

// A value through the loads of the locals it was stored in, as the lowering reads them.
static uint32_t stored_value(const XscLower *l, uint32_t ssa)
{
	for (uint32_t depth = 0; depth < XSC_DEPTH_LIMIT; depth++) {
		const XscInstr *const load = xsdk_xsc_by_result(l->m, ssa);
		const uint32_t pointer = load && load->opcode == SPV_LOAD ? xsdk_xsc_operand(load, 0) : 0;
		if (!pointer || pointer >= l->m->id_limit || !l->local_store[pointer])
			break;
		ssa = l->local_store[pointer];
	}
	return ssa;
}

// Whether a value is gl_Position's: the value stored into it, or a load of it.
static bool is_position(XscLower *l, const PerVertex *pv, uint32_t ssa, uint32_t position)
{
	const uint32_t value = stored_value(l, ssa);
	if (value == stored_value(l, position))
		return true;
	const XscInstr *const load = xsdk_xsc_by_result(l->m, value);
	return load && load->opcode == SPV_LOAD && is_position_pointer(l, pv, xsdk_xsc_operand(load, 0));
}

// The register of the plane a clip distance reads: its value is dot(gl_Position, plane), either way round, the plane a
// whole vec4 of a uniform or push constant block at a fixed index.
static bool plane_register(XscLower *l, const PerVertex *pv, uint32_t value, uint32_t position, int64_t *reg)
{
	const XscInstr *const dot = xsdk_xsc_by_result(l->m, stored_value(l, value));
	const uint32_t a = dot && dot->opcode == SPV_DOT ? xsdk_xsc_operand(dot, 0) : 0;
	const uint32_t b = a ? xsdk_xsc_operand(dot, 1) : 0;
	const uint32_t plane = !a ? 0 : is_position(l, pv, a, position) ? b : is_position(l, pv, b, position) ? a : 0;
	const XscInstr *const load = plane ? xsdk_xsc_by_result(l->m, stored_value(l, plane)) : NULL;
	XscBlockAccess access;
	bool found = false;
	if (load && load->opcode == SPV_LOAD && !xsdk_xsc_block_access(l, xsdk_xsc_operand(load, 0), &access, &found))
		return false;
	const XscType *const t = found ? xsdk_xsc_type(l->m, l->types, access.tid) : NULL;
	if (!t || t->kind != XSC_TYPE_VECTOR || t->count != 4 || access.comp || access.dynamic)
		return xsdk_xsc_lower_refuse(l,
			"gl_ClipDistance other than dot(gl_Position, a plane), the plane a whole vec4 of a block at a fixed "
			"index, as Xenos's user clip planes take it (v1)");
	*reg = access.reg;
	return true;
}

// gl_ClipDistance[n] = dot(gl_Position, plane) is Xenos's user clip plane n, which clips against the exported position
// itself (xenia:src/xenia/gpu/dxbc_shader_translator.cc:1033-1053), so it costs no instruction: the runtime sets the
// plane from its register. Xenos has no cull distances.
static bool clip_planes(XscLower *l, const PerVertex *pv, const XscStores *stores, uint32_t position, XscLowered *out)
{
	uint32_t written = 0;
	for (size_t i = 0; i < stores->target_count && pv->position != XSC_NONE; i++) {
		const uint32_t target = stores->targets[i];
		const int64_t member = member_of(l, target);
		if (xsc_failed(l))
			return false;
		if (member != XSC_NONE && member == pv->cull_distance)
			return xsdk_xsc_lower_refuse(l, "gl_CullDistance: Xenos has no cull distances");
		if (member == XSC_NONE || member != pv->clip_distance)
			continue;
		const XscInstr *const chain = xsdk_xsc_by_result(l->m, target);
		int64_t n = XSC_NONE;
		if (xsdk_xsc_operand_count(chain) != 3 || !xsdk_xsc_int_const(l, xsdk_xsc_operand(chain, 2), &n) || n < 0 ||
			n >= CLIP_PLANES || (written & 1u << n))
			return xsdk_xsc_lower_refuse(
				l, "gl_ClipDistance other than elements 0 to 5 each written once at a constant index (v1)");
		int64_t reg = 0;
		if (!plane_register(l, pv, stores->value[target], position, &reg))
			return false;
		written |= 1u << n;
		const XsdkXscNeed need = { .kind = XSDK_XSC_CLIP_PLANE, .reg = (uint32_t)reg, .regs = (uint32_t)n };
		if (!xsdk_xsc_add_need(l->x, out, need))
			return false;
	}
	return true;
}

// An output decorated as a gl_PerVertex member but standing alone would be dropped, so it's refused.
static bool check_lone_outputs(Xsc *x, const XscModule *m)
{
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode != SPV_VARIABLE || xsdk_xsc_operand(ins, 0) != STORAGE_OUTPUT)
			continue;
		const int64_t builtin =
			xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, ins->result_id, SPV_DEC_BUILTIN), XSC_NONE);
		if (builtin == BUILTIN_CLIP_DISTANCE || builtin == BUILTIN_CULL_DISTANCE)
			return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "gl_ClipDistance or gl_CullDistance outside gl_PerVertex (v1)");
	}
	return true;
}

bool xsdk_xsc_lower_vertex(Xsc *x, const XscModule *m, const XsdkXscOptions *options, XscInterface *si, XscLowered *out)
{
	if (!xsdk_xsc_resolve_interface(x, m, si))
		return false;
	if (si->pixel)
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "lower_vertex called on non-vertex module");
	if (has_branches(m))
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "vertex shader control flow the unroller left isn't lowered");
	if (!check_lone_outputs(x, m))
		return false;
	const XscType *const types = xsdk_xsc_resolve_types(x, m);
	XscStores stores;
	XscProgram p;
	if (!types || !xsdk_xsc_store_map(x, m, &stores) || !xsdk_xsc_program_init(x, &p, false, options->vertex_registers))
		return false;
	p.optimize = options->optimize;
	// Temps run past r63 till the allocator packs them.
	p.whole_sources = true;
	XscLower l;
	const int64_t first_reg = (int64_t)si->vfetch_count;
	if (!xsdk_xsc_lower_init(x, &l, m, si, &p, types, true, first_reg, xsdk_xsc_literal_base(si)) ||
		!fetch_inputs(&l, si))
		return false;
	bool *const locals = xsdk_xsc_local_vars(x, m, 0, m->instr_count);
	if (!locals || !xsdk_xsc_check_local_stores(x, m, locals, 0, 0))
		return false;
	xsdk_xsc_fill_local_store(&l, &stores, locals);
	bool used[FETCH_SLOTS] = { false };
	XscConstants constants = { 0 };
	const PerVertex pv = per_vertex(m, types, si);
	const uint32_t position = position_value(&l, &pv, &stores);
	return !xsc_failed(&l) && sample_all(&l, used) && lower_body(&l, si, &stores, position) &&
		add_constants(&l, si, used, &constants) &&
		xsdk_xsc_program_finish(x, &p, constants.items, constants.count, &out->container, &out->container_bytes) &&
		xsdk_xsc_literal_needs(&l, out) && xsdk_xsc_loop_needs(&l, out) && clip_planes(&l, &pv, &stores, position, out);
}
