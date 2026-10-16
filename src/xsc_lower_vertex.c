#include "xsc_lower.h"

// A vertex shader, lowered as lower.py's lower_vertex lowers it.

// The rows and columns a matrix constant declares.
enum {
	MATRIX_SIZE = 4,
};

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

static uint32_t position_value(const XscModule *m, const XscInterface *si, const XscStores *stores)
{
	uint32_t value = 0;
	for (size_t i = 0; i < stores->target_count; i++) {
		const uint32_t target = stores->targets[i];
		const XscInstr *const chain = xsdk_xsc_by_result(m, target);
		const bool into = chain && chain->opcode == SPV_ACCESS_CHAIN && xsdk_xsc_operand(chain, 0) == si->position_out;
		if (target == si->position_out || into)
			value = stores->value[target];
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

static bool add_constants(XscLower *l, const XscInterface *si, XscConstants *constants)
{
	for (size_t i = 0; i < si->constant_count; i++) {
		const XscConstantSlot *const cs = &si->constants[i];
		XscConstant c = xsdk_xsc_vector_constant(cs->name, cs->reg, 1);
		if (cs->regs > 1)
			c = (XscConstant){ cs->name, XSC_REGISTER_SET_FLOAT4, cs->reg, cs->regs, XSC_CLASS_MATRIX_COLUMNS,
				XSC_TYPE_FLOAT_PARAMETER, MATRIX_SIZE, MATRIX_SIZE };
		if (!xsdk_xsc_add_constant(l->x, constants, c))
			return false;
	}
	return xsdk_xsc_literal_constants(l, constants);
}

// The position, then the interpolators; without any, they're allocated first, as xsdk's resolve shader is.
static bool lower_body(XscLower *l, const XscInterface *si, const XscStores *stores)
{
	if (!si->interp_out_count && !xsdk_xsc_program_alloc_interpolators(l->x, l->p))
		return false;
	if (!xsdk_xsc_program_alloc(l->x, l->p, XSC_ALLOC_POSITION))
		return false;
	const uint32_t position = position_value(l->m, si, stores);
	if (!position)
		return xsdk_xsc_lower_refuse(l, "no write to gl_Position");
	if (!xsdk_xsc_emit_value(l, position, XSC_EXPORT_POSITION, true, true))
		return false;
	if (si->interp_out_count && !xsdk_xsc_program_alloc_interpolators(l->x, l->p))
		return false;
	return export_interpolators(l, si, stores);
}

bool xsdk_xsc_lower_vertex(Xsc *x, const XscModule *m, const XsdkXscOptions *options, XscInterface *si, XscLowered *out)
{
	if (!xsdk_xsc_resolve_interface(x, m, si))
		return false;
	if (si->pixel)
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "lower_vertex called on non-vertex module");
	if (has_branches(m))
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "vertex shader control flow the unroller left isn't lowered");
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
	XscConstants constants = { 0 };
	return lower_body(&l, si, &stores) && add_constants(&l, si, &constants) &&
		xsdk_xsc_program_finish(x, &p, constants.items, constants.count, &out->container, &out->container_bytes) &&
		xsdk_xsc_literal_needs(&l, out) && xsdk_xsc_loop_needs(&l, out);
}
