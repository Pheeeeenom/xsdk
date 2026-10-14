#include "xsc_lower.h"

// A pixel shader, lowered as lower.py's lower_fragment lowers it: samples and derivatives first, then one value, or an
// if/else of them as cnd selects, or a discard, to the color.

enum {
	// getGradients' .xzxz is ddx(src.xy) and .ywyw ddy(src.xy).
	GRADIENT_X = 0xE4,
	GRADIENT_Y = 0x39,
	// The parameter type xost's containers give a sampler.
	SAMPLER_PARAMETER = 0xC,
	IMAGE_OPERANDS_BIAS_OR_LOD = 0x3,
	DIM_2D = 1,
	DIM_CUBE = 3,
};

typedef struct Region Region;

// A value one if/else arm stores, or an if/else of two regions: the select's comparison, NULL for a value.
struct Region {
	const XscInstr *cmp;
	uint32_t value;
	const Region *on_true;
	const Region *on_false;
};

typedef struct {
	XscLower l;
	const XscStores *stores;
	const bool *locals;
	bool *gathered;
	uint32_t out_var;
	uint32_t out_value;
	// The local the if/else assigns, read after it merges; 0 for none.
	uint32_t merged_local;
	const Region *region;
	uint32_t region_target;
	const XscInstr *kill_cmp;
	bool kill_on_true;
	int64_t *slots;
	size_t slot_count;
	size_t slot_capacity;
} Fragment;

// How a comparison selects (cnd), and kills where it holds and where it doesn't (lower.py _CMP, _KILL_TRUE,
// _KILL_FALSE).
typedef struct {
	uint32_t opcode;
	int64_t cnd;
	bool cnd_swap_operands;
	bool cnd_swap_arms;
	int64_t kill_true;
	bool kill_true_swap;
	int64_t kill_false;
	bool kill_false_swap;
} Comparison;

static const Comparison comparisons[] = {
	{ SPV_F_ORD_GREATER_THAN, XSC_VECTOR_CND_GT, false, false, XSC_VECTOR_KILL_GT, false, XSC_VECTOR_KILL_GE, true },
	{ SPV_F_ORD_LESS_THAN, XSC_VECTOR_CND_GT, true, false, XSC_VECTOR_KILL_GT, true, XSC_VECTOR_KILL_GE, false },
	{ SPV_F_ORD_GREATER_THAN_EQUAL, XSC_VECTOR_CND_GE, false, false, XSC_VECTOR_KILL_GE, false, XSC_VECTOR_KILL_GT,
		true },
	{ SPV_F_ORD_LESS_THAN_EQUAL, XSC_VECTOR_CND_GE, true, false, XSC_VECTOR_KILL_GE, true, XSC_VECTOR_KILL_GT, false },
	{ SPV_F_ORD_EQUAL, XSC_VECTOR_CND_EQ, false, false, XSC_VECTOR_KILL_EQ, false, XSC_VECTOR_KILL_NE, false },
	{ SPV_F_ORD_NOT_EQUAL, XSC_VECTOR_CND_EQ, false, true, XSC_VECTOR_KILL_NE, false, XSC_VECTOR_KILL_EQ, false },
};

static const Comparison *comparison(uint32_t opcode)
{
	for (size_t i = 0; i < sizeof comparisons / sizeof comparisons[0]; i++) {
		if (comparisons[i].opcode == opcode)
			return &comparisons[i];
	}
	return NULL;
}

static bool is_local(const Fragment *f, uint32_t id)
{
	return id < f->l.m->id_limit && f->locals[id];
}

static int64_t origin_swizzle(XscOrigin o)
{
	return o.comp == XSC_BROADCAST ? 0 : xsdk_xsc_bsw(o.comp);
}

// Whether any use reads gl_FragCoord's z or w: through any use but one component's, all four (lower.py
// _frag_coord_reads).
static bool reads_depth(Xsc *x, const XscModule *m, const XscType *types, uint32_t var, bool *depth)
{
	bool *const loads = xsdk_xsc_alloc(x, m->id_limit);
	if (!loads)
		return false;
	*depth = false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode == SPV_LOAD && xsdk_xsc_operand(ins, 0) == var && ins->result_id < m->id_limit) {
			loads[ins->result_id] = true;
		} else if (ins->opcode == SPV_ACCESS_CHAIN && xsdk_xsc_operand(ins, 0) == var) {
			const XscInstr *const c = xsdk_xsc_by_result(m, xsdk_xsc_operand(ins, 1));
			const uint32_t comp = c && c->opcode == SPV_CONSTANT ? xsdk_xsc_operand(c, 0) : 2;
			*depth = *depth || comp == 2 || comp == 3;
		}
	}
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		uint32_t count;
		const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
		const bool load0 = count > 0 && ops[0] < m->id_limit && loads[ops[0]];
		if (ins->opcode == SPV_COMPOSITE_EXTRACT) {
			*depth = *depth || (load0 && count > 1 && (ops[1] == 2 || ops[1] == 3));
			continue;
		}
		if (ins->opcode == SPV_VECTOR_SHUFFLE) {
			const XscInstr *const first = xsdk_xsc_by_result(m, xsdk_xsc_operand(ins, 0));
			if (!first)
				return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "%u", (unsigned)xsdk_xsc_operand(ins, 0));
			const XscType *const t = xsdk_xsc_type(m, types, first->result_type);
			const uint32_t size = t && t->kind == XSC_TYPE_VECTOR ? t->count : 1;
			const bool load1 = count > 1 && ops[1] < m->id_limit && loads[ops[1]];
			for (uint32_t k = 2; k < count; k++) {
				const uint32_t c = ops[k];
				*depth = *depth || (load0 && (c == 2 || c == 3)) || (load1 && (c == size + 2 || c == size + 3));
			}
			continue;
		}
		for (uint32_t k = ins->opcode == SPV_EXT_INST ? 2 : 0; k < count; k++)
			*depth = *depth || (ops[k] < m->id_limit && loads[ops[k]]);
	}
	return true;
}

// Vulkan's gl_FragCoord is the pixel's centre: the column and row, |xy| as the sign bits are flags, plus 0.5.
static bool frag_coord_register(XscLower *l, const XscInterface *si, int64_t param_reg)
{
	const int64_t coord = xsdk_xsc_fresh(l);
	const int64_t half = xsdk_xsc_literal_splat(l, 0.5);
	if (half == XSC_NONE)
		return false;
	const XscSrc params = xsc_temp(param_reg | XSC_ABS_SOURCE);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, params, xsc_plain(xsc_const(half)), coord, false, 0x3))
		return false;
	l->inputs[si->frag_coord->vid] = coord;
	return true;
}

static bool gather(Fragment *f, uint32_t ssa);

static bool gather_derivative(Fragment *f, uint32_t ssa, const XscInstr *ins)
{
	XscLower *const l = &f->l;
	const uint32_t value = xsdk_xsc_operand(ins, 0);
	if (!gather(f, value))
		return false;
	if (l->deriv_reg[ssa] != XSC_NONE)
		return true;
	const XscOperand src = xsdk_xsc_to_reg(l, xsdk_xsc_lower_operand(l, value));
	if (xsc_failed(l))
		return false;
	const int64_t grad = xsdk_xsc_fresh(l);
	XscFetch t = xsdk_xsc_template_tfetch();
	t.opcode = XSC_FETCH_GET_GRADIENTS;
	t.src_swizzle = 0 | 1 << 2;
	t.dest = grad;
	t.src = src.value;
	if (!xsdk_xsc_program_tfetch(l->x, l->p, &t))
		return false;
	int64_t r;
	if (ins->opcode == SPV_DPDX || ins->opcode == SPV_DPDY) {
		r = xsdk_xsc_fresh(l);
		const XscSrc s = xsc_src(xsc_reg(grad), false, ins->opcode == SPV_DPDX ? GRADIENT_X : GRADIENT_Y);
		if (!xsc_emit2(l, XSC_VECTOR_MAX, s, s, r, false, 0xF))
			return false;
	} else {
		// fwidth = |ddx| + |ddy|.
		const int64_t ag = xsdk_xsc_fresh(l);
		if (!xsc_emit2(l, XSC_VECTOR_MAX, xsc_temp(grad), xsc_negated(xsc_reg(grad)), ag, false, 0xF))
			return false;
		r = xsdk_xsc_fresh(l);
		const XscSrc x = xsc_src(xsc_reg(ag), false, GRADIENT_X);
		if (!xsc_emit2(l, XSC_VECTOR_ADD, x, xsc_src(xsc_reg(ag), false, GRADIENT_Y), r, false, 0xF))
			return false;
	}
	l->deriv_reg[ssa] = r;
	return true;
}

static bool sample_cube(XscLower *l, int64_t coord, int64_t slot, double lod_bias, int64_t dst)
{
	static const int64_t zzxy[] = { 2, 2, 0, 1 };
	static const int64_t yxzz[] = { 1, 0, 2, 2 };
	const int64_t cube = xsdk_xsc_fresh(l);
	const XscSrc a = xsc_src(xsc_reg(coord), false, xsdk_xsc_swz(zzxy, 4));
	const XscSrc b = xsc_src(xsc_reg(coord), false, xsdk_xsc_swz(yxzz, 4));
	if (!xsc_emit2(l, XSC_VECTOR_CUBE, a, b, cube, false, 0xF) ||
		xsdk_xsc_emit_scalar(l, XSC_SCALAR_RCP, cube, xsc_reg(cube | XSC_ABS_SOURCE), 2, 0x4) == XSC_NONE)
		return false;
	const int64_t bias = xsdk_xsc_literal_splat(l, 1.5);
	const XscSrc ma = xsc_src(xsc_reg(cube), false, xsdk_xsc_bsw(2));
	if (bias == XSC_NONE ||
		!xsc_emit3(l, XSC_VECTOR_MAD, xsc_temp(cube), ma, xsc_plain(xsc_const(bias)), cube, false, 0x3))
		return false;
	XscFetch t = xsdk_xsc_template_tfetch();
	t.fetch_constant_index = slot;
	t.dimension = XSC_DIMENSION_CUBE;
	t.lod_bias = lod_bias;
	t.src_swizzle = 1 | 0 << 2 | 3 << 4;
	t.dest = dst;
	t.src = cube;
	return xsdk_xsc_program_tfetch(l->x, l->p, &t) != NULL;
}

// A projective sample divides the coordinate by its last component first.
static bool sample_2d(
	XscLower *l, uint32_t coord, int64_t coord_reg, bool proj, int64_t slot, double lod_bias, int64_t dst)
{
	if (proj) {
		const int64_t size = xsdk_xsc_vec_size(l, coord);
		const int64_t rw = xsdk_xsc_emit_scalar(
			l, XSC_SCALAR_RCP, xsdk_xsc_fresh(l), xsc_reg(coord_reg), size - 1 > 1 ? size - 1 : 1, 0xF);
		if (rw == XSC_NONE)
			return false;
		const int64_t uv = xsdk_xsc_fresh(l);
		if (!xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(coord_reg), xsc_temp(rw), uv, false, 0xF))
			return false;
		coord_reg = uv;
	}
	XscFetch t = xsdk_xsc_template_tfetch();
	t.fetch_constant_index = slot;
	t.lod_bias = lod_bias;
	t.dest = dst;
	t.src = coord_reg;
	return xsdk_xsc_program_tfetch(l->x, l->p, &t) != NULL;
}

static bool use_slot(Fragment *f, int64_t slot)
{
	for (size_t i = 0; i < f->slot_count; i++) {
		if (f->slots[i] == slot)
			return true;
	}
	if (!xsdk_xsc_room(f->l.x, &f->slots, f->slot_count, &f->slot_capacity, sizeof *f->slots))
		return false;
	f->slots[f->slot_count++] = slot;
	return true;
}

// A tfetch, its coordinate's work and any sample that coordinate reads first.
static bool gather_sample(Fragment *f, uint32_t ssa, const XscInstr *ins)
{
	XscLower *const l = &f->l;
	const bool proj =
		ins->opcode == SPV_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD || ins->opcode == SPV_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD;
	const XscInstr *const sampled = xsdk_xsc_by_result(l->m, xsdk_xsc_operand(ins, 0));
	const XscRole *const role = sampled ? xsdk_xsc_role(l->si, xsdk_xsc_operand(sampled, 0)) : NULL;
	if (!role || role->kind != XSC_ROLE_SAMPLER)
		return xsdk_xsc_lower_refuse(l, "ImageSample needs a sampler");
	const int64_t slot = role->a;
	const int64_t dim = role->b;
	if (dim == XSC_NONE)
		return xsdk_xsc_lower_refuse(l, "unsupported sampler dimension None");
	if (dim != DIM_2D && dim != DIM_CUBE)
		return xsdk_xsc_lower_refuse(l, "unsupported sampler dimension %lld", (long long)dim);
	const uint32_t coord = xsdk_xsc_operand(ins, 1);
	// A constant bias or LOD folds into the tfetch's.
	double lod_bias = 0.0;
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	if (count > 3 && (ops[2] & IMAGE_OPERANDS_BIAS_OR_LOD) && !xsdk_xsc_const_scalar(l, ops[3], &lod_bias))
		return false;
	if (!gather(f, coord))
		return false;
	const XscOperand coord_reg = xsdk_xsc_to_reg(l, xsdk_xsc_lower_operand(l, coord));
	if (xsc_failed(l))
		return false;
	const int64_t dst = xsdk_xsc_fresh(l);
	if (dim == DIM_CUBE && proj)
		return xsdk_xsc_lower_refuse(l, "projective cube sampling unsupported");
	const bool sampled_ok = dim == DIM_CUBE ? sample_cube(l, coord_reg.value, slot, lod_bias, dst)
											: sample_2d(l, coord, coord_reg.value, proj, slot, lod_bias, dst);
	if (!sampled_ok)
		return false;
	l->sample_reg[ssa] = dst;
	return use_slot(f, slot);
}

static bool gather_value(Fragment *f, uint32_t ssa)
{
	const XscModule *const m = f->l.m;
	const XscInstr *const ins = xsdk_xsc_by_result(m, ssa);
	if (!ins)
		return true;
	const uint32_t op = ins->opcode;
	if (op == SPV_DPDX || op == SPV_DPDY || op == SPV_FWIDTH)
		return gather_derivative(f, ssa, ins);
	if (op == SPV_IMAGE_SAMPLE_IMPLICIT_LOD || op == SPV_IMAGE_SAMPLE_EXPLICIT_LOD ||
		op == SPV_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD || op == SPV_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD)
		return gather_sample(f, ssa, ins);
	const uint32_t base = xsdk_xsc_operand(ins, 0);
	const uint32_t stored = base < m->id_limit ? f->l.local_store[base] : 0;
	if ((op == SPV_LOAD || op == SPV_ACCESS_CHAIN) && f->merged_local && base == f->merged_local)
		return true;
	if (op == SPV_LOAD && stored)
		return gather(f, stored);
	if (op == SPV_ACCESS_CHAIN)
		return !stored || gather(f, stored);
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	for (uint32_t i = op == SPV_EXT_INST ? 2 : 0; i < count; i++) {
		if (xsdk_xsc_by_result(m, ops[i]) && !gather(f, ops[i]))
			return false;
	}
	return true;
}

// Every sample and derivative a value reads, emitted before the colors' alloc as fetches go there.
static bool gather(Fragment *f, uint32_t ssa)
{
	if (ssa < f->l.m->id_limit) {
		if (f->gathered[ssa])
			return true;
		f->gathered[ssa] = true;
	}
	if (!xsdk_xsc_enter(&f->l))
		return false;
	const bool ok = gather_value(f, ssa);
	f->l.depth--;
	return ok;
}

// The instructions after a label, through its block's terminator.
static void block_span(const XscModule *m, uint32_t label, size_t *start, size_t *end)
{
	const XscInstr *const ins = xsdk_xsc_by_result(m, label);
	*start = *end = 0;
	if (!ins || ins->opcode != SPV_LABEL)
		return;
	size_t i = (size_t)(ins - m->instrs) + 1;
	*start = i;
	while (i < m->instr_count) {
		const uint32_t op = m->instrs[i++].opcode;
		if (op == SPV_BRANCH || op == SPV_BRANCH_CONDITIONAL || op == SPV_KILL || op == SPV_RETURN ||
			op == SPV_RETURN_VALUE)
			break;
	}
	*end = i;
}

static bool has_kill(const XscModule *m, uint32_t label)
{
	size_t start, end;
	block_span(m, label, &start, &end);
	for (size_t i = start; i < end; i++) {
		if (m->instrs[i].opcode == SPV_KILL)
			return true;
	}
	return false;
}

static const Region *leaf(Fragment *f, uint32_t value)
{
	Region *const r = xsdk_xsc_alloc(f->l.x, sizeof *r);
	if (r)
		r->value = value;
	return r;
}

// An arm's last store to target, or with none given, to the one output or local it stores.
static bool leaf_region(Fragment *f, size_t start, size_t end, uint32_t target, const Region **out, uint32_t *found)
{
	const XscModule *const m = f->l.m;
	uint32_t chosen = target;
	bool several = false;
	uint32_t value = 0;
	for (size_t i = start; i < end; i++) {
		const XscInstr *const ins = &m->instrs[i];
		const uint32_t t = xsdk_xsc_operand(ins, 0);
		if (ins->opcode != SPV_STORE || (!target && t != f->out_var && !is_local(f, t)))
			continue;
		if (!chosen)
			chosen = t;
		several = several || t != chosen;
		if (t == chosen)
			value = xsdk_xsc_operand(ins, 1);
	}
	if (target && !value && target != f->out_var)
		return xsdk_xsc_lower_refuse(&f->l, "if/else arm does not assign %%%u", (unsigned)target);
	if (!value || (!target && several))
		return xsdk_xsc_lower_refuse(&f->l, "if/else arm does not assign o_color");
	*out = leaf(f, value);
	*found = chosen;
	return *out != NULL;
}

static bool region_at(Fragment *f, uint32_t label, uint32_t target, const Region **out, uint32_t *found);

// An if/else whose arms assign the same variable, nested ones as selects of selects.
static bool selection(Fragment *f, const XscInstr *brc, uint32_t target, const Region **out, uint32_t *found)
{
	XscLower *const l = &f->l;
	if (xsdk_xsc_operand_count(brc) != 3)
		return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "a branch with weights");
	const Region *on_true, *on_false;
	uint32_t true_target, false_target;
	if (!region_at(f, xsdk_xsc_operand(brc, 1), target, &on_true, &true_target) ||
		!region_at(f, xsdk_xsc_operand(brc, 2), target ? target : true_target, &on_false, &false_target))
		return false;
	if (true_target != false_target)
		return xsdk_xsc_lower_refuse(l, "if/else arms assign different variables");
	const XscInstr *const cmp = xsdk_xsc_lower_result(l, xsdk_xsc_operand(brc, 0));
	Region *const r = cmp ? xsdk_xsc_alloc(l->x, sizeof *r) : NULL;
	if (!r)
		return false;
	*r = (Region){ cmp, 0, on_true, on_false };
	*out = r;
	*found = true_target;
	return true;
}

static bool region_in(Fragment *f, uint32_t label, uint32_t target, const Region **out, uint32_t *found)
{
	const XscModule *const m = f->l.m;
	size_t start, end;
	block_span(m, label, &start, &end);
	for (size_t i = start; i < end; i++) {
		if (m->instrs[i].opcode == SPV_BRANCH_CONDITIONAL)
			return selection(f, &m->instrs[i], target, out, found);
	}
	return leaf_region(f, start, end, target, out, found);
}

static bool region_at(Fragment *f, uint32_t label, uint32_t target, const Region **out, uint32_t *found)
{
	if (!xsdk_xsc_enter(&f->l))
		return false;
	const bool ok = region_in(f, label, target, out, found);
	f->l.depth--;
	return ok;
}

static bool gather_region(Fragment *f, const Region *r)
{
	if (!r->cmp)
		return gather(f, r->value);
	return gather(f, xsdk_xsc_operand(r->cmp, 0)) && gather(f, xsdk_xsc_operand(r->cmp, 1)) &&
		gather_region(f, r->on_true) && gather_region(f, r->on_false);
}

static bool gather_output(Fragment *f)
{
	const uint32_t value = f->stores->value[f->out_var];
	if (!value)
		return xsdk_xsc_lower_refuse(&f->l, "o_color never written");
	f->out_value = value;
	return gather(f, value);
}

static bool plan_kill(Fragment *f, const XscInstr *top, bool true_kills, bool false_kills)
{
	if (true_kills && false_kills)
		return xsdk_xsc_lower_refuse(&f->l, "if/discard kills both arms");
	f->kill_cmp = xsdk_xsc_lower_result(&f->l, xsdk_xsc_operand(top, 0));
	f->kill_on_true = true_kills;
	return f->kill_cmp && gather(f, xsdk_xsc_operand(f->kill_cmp, 0)) && gather(f, xsdk_xsc_operand(f->kill_cmp, 1)) &&
		gather_output(f);
}

// An if/else assigns the color itself, or a local the color reads after it merges.
static bool plan_selection(Fragment *f, const XscInstr *top)
{
	if (!selection(f, top, 0, &f->region, &f->region_target) || !gather_region(f, f->region))
		return false;
	if (f->region_target == f->out_var)
		return true;
	if (!is_local(f, f->region_target))
		return xsdk_xsc_lower_refuse(&f->l, "if/else arms assign unsupported variable");
	f->merged_local = f->region_target;
	return gather_output(f);
}

// The module's first if/else picks the shape: none, a discard, or a selection.
static bool plan(Fragment *f, const XscInstr *top)
{
	if (!top)
		return gather_output(f);
	if (xsdk_xsc_operand_count(top) != 3)
		return xsdk_xsc_fail(f->l.x, XSDK_XSC_MALFORMED, "a branch with weights");
	const bool true_kills = has_kill(f->l.m, xsdk_xsc_operand(top, 1));
	const bool false_kills = has_kill(f->l.m, xsdk_xsc_operand(top, 2));
	if (true_kills || false_kills)
		return plan_kill(f, top, true_kills, false_kills);
	return plan_selection(f, top);
}

// The if/else's merge block, named by the OpSelectionMerge just before its branch.
static uint32_t merge_label(const XscModule *m, const XscInstr *top)
{
	if (!top)
		return 0;
	const size_t index = (size_t)(top - m->instrs);
	const XscInstr *const before = &m->instrs[index ? index - 1 : m->instr_count - 1];
	return before->opcode == SPV_SELECTION_MERGE ? xsdk_xsc_operand(before, 0) : 0;
}

static bool compared(XscLower *l, const XscInstr *cmp, bool swap, XscOrigin *a, XscOrigin *b)
{
	*a = xsdk_xsc_scalar_origin(l, xsdk_xsc_operand(cmp, 0));
	if (xsc_failed(l))
		return false;
	*b = xsdk_xsc_scalar_origin(l, xsdk_xsc_operand(cmp, 1));
	if (xsc_failed(l))
		return false;
	if (swap) {
		const XscOrigin t = *a;
		*a = *b;
		*b = t;
	}
	return true;
}

static bool emit_kill(Fragment *f)
{
	XscLower *const l = &f->l;
	const Comparison *const c = comparison(f->kill_cmp->opcode);
	char name[16];
	if (!c)
		return xsdk_xsc_lower_refuse(
			l, "v1: unsupported discard comparison %s", xsdk_xsc_op_name(f->kill_cmp->opcode, name, sizeof name));
	const int64_t op = f->kill_on_true ? c->kill_true : c->kill_false;
	XscOrigin a, b;
	if (!compared(l, f->kill_cmp, f->kill_on_true ? c->kill_true_swap : c->kill_false_swap, &a, &b))
		return false;
	return xsc_emit2(
		l, op, xsc_src(a.o, false, origin_swizzle(a)), xsc_src(b.o, false, origin_swizzle(b)), 0, false, 0);
}

static bool emit_region(Fragment *f, const Region *region, int64_t dst, bool export_);

// A leaf through operand(), which takes no extra mov, or where it refuses, say a general construct, spilled to a temp.
static XscOperand region_value(Fragment *f, const Region *region)
{
	XscLower *const l = &f->l;
	if (region->cmp) {
		const int64_t r = xsdk_xsc_fresh(l);
		return emit_region(f, region, r, false) ? xsc_reg(r) : (XscOperand){ 0 };
	}
	const XscOperand o = xsdk_xsc_lower_operand(l, region->value);
	if (l->x->status != XSDK_XSC_REFUSED)
		return o;
	l->x->status = XSDK_XSC_OK;
	l->x->message[0] = '\0';
	const int64_t r = xsdk_xsc_fresh(l);
	return xsdk_xsc_emit_value(l, region->value, r, false, false) ? xsc_reg(r) : (XscOperand){ 0 };
}

// cnd(a - b, the true arm's value, the false arm's).
static bool emit_region(Fragment *f, const Region *region, int64_t dst, bool export_)
{
	XscLower *const l = &f->l;
	if (!region->cmp)
		return xsdk_xsc_emit_value(l, region->value, dst, export_, false);
	const Comparison *const c = comparison(region->cmp->opcode);
	char name[16];
	if (!c)
		return xsdk_xsc_lower_refuse(
			l, "v1: unsupported comparison %s", xsdk_xsc_op_name(region->cmp->opcode, name, sizeof name));
	XscOrigin a, b;
	if (!compared(l, region->cmp, c->cnd_swap_operands, &a, &b))
		return false;
	const Region *const on_true = c->cnd_swap_arms ? region->on_false : region->on_true;
	const Region *const on_false = c->cnd_swap_arms ? region->on_true : region->on_false;
	const int64_t d = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_src(a.o, false, origin_swizzle(a)), xsc_src(b.o, true, origin_swizzle(b)), d,
			false, 0xF))
		return false;
	const XscOperand tv = region_value(f, on_true);
	if (xsc_failed(l))
		return false;
	XscOperand fv = region_value(f, on_false);
	if (xsc_failed(l))
		return false;
	// cnd takes one constant register.
	if (tv.kind == XSC_OPERAND_CONST && fv.kind == XSC_OPERAND_CONST) {
		fv = xsdk_xsc_to_reg(l, fv);
		if (xsc_failed(l))
			return false;
	}
	return xsc_emit3(l, c->cnd, xsc_temp(d), xsc_plain(tv), xsc_plain(fv), dst, export_, 0xF);
}

static bool emit_output(Fragment *f)
{
	XscLower *const l = &f->l;
	if (f->kill_cmp && !emit_kill(f))
		return false;
	if (f->region && f->region_target == f->out_var)
		return emit_region(f, f->region, 0, true);
	if (f->region) {
		const int64_t r = xsdk_xsc_fresh(l);
		if (!emit_region(f, f->region, r, false))
			return false;
		l->local_values[f->merged_local] = xsc_reg(r);
	}
	return xsdk_xsc_emit_value(l, f->out_value, 0, true, false);
}

static bool add_constants(Fragment *f, XscConstants *constants)
{
	XscLower *const l = &f->l;
	for (size_t i = 1; i < f->slot_count; i++) {
		const int64_t slot = f->slots[i];
		size_t j = i;
		for (; j > 0 && f->slots[j - 1] > slot; j--)
			f->slots[j] = f->slots[j - 1];
		f->slots[j] = slot;
	}
	for (size_t i = 0; i < f->slot_count; i++) {
		const int64_t slot = f->slots[i];
		const char *const name = xsdk_xsc_format(l->x, "tex%lld", (long long)slot);
		const XscConstant c = { name, XSC_REGISTER_SET_SAMPLER, (uint32_t)slot, 1, XSC_CLASS_OBJECT, SAMPLER_PARAMETER,
			1, 1 };
		if (!name || !xsdk_xsc_add_constant(l->x, constants, c))
			return false;
	}
	for (size_t i = 0; i < l->si->constant_count; i++) {
		const XscConstantSlot *const cs = &l->si->constants[i];
		if (!xsdk_xsc_add_constant(l->x, constants, xsdk_xsc_vector_constant(cs->name, cs->reg, cs->regs)))
			return false;
	}
	return xsdk_xsc_literal_constants(l, constants);
}

// The interpolators from r0, then gl_FragCoord's pixel parameters past them; the highest register they take.
static bool declare_inputs(Xsc *x, const XscModule *m, const XscType *types, XscProgram *p, const XscInterface *si,
	int64_t *highest, int64_t *param_reg)
{
	*highest = -1;
	*param_reg = XSC_NONE;
	for (size_t i = 0; i < si->interp_in_count; i++) {
		const XscVarUse *const use = &si->interp_in[i];
		if (!xsdk_xsc_program_input(x, p, use->a, use->b))
			return false;
		if ((int64_t)use->a > *highest)
			*highest = use->a;
	}
	if (!si->frag_coord)
		return true;
	bool depth;
	if (!reads_depth(x, m, types, si->frag_coord->vid, &depth))
		return false;
	if (depth)
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "gl_FragCoord.zw: the pixel parameters hold no depth (v1)");
	*param_reg = *highest + 1;
	*highest = *param_reg;
	return xsdk_xsc_program_pixel_parameters(x, p, *param_reg);
}

static bool lower_body(Fragment *f, int64_t param_reg, XscConstants *constants, XscLowered *out)
{
	XscLower *const l = &f->l;
	const XscInterface *const si = l->si;
	for (size_t i = 0; i < si->interp_in_count; i++)
		l->inputs[si->interp_in[i].var->vid] = si->interp_in[i].a;
	if (param_reg != XSC_NONE && !frag_coord_register(l, si, param_reg))
		return false;
	if (!si->color_out_count)
		return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "list index out of range");
	f->out_var = si->color_out[0].var->vid;
	const XscInstr *top = NULL;
	for (size_t i = 0; !top && i < l->m->instr_count; i++)
		top = l->m->instrs[i].opcode == SPV_BRANCH_CONDITIONAL ? &l->m->instrs[i] : NULL;
	if (!plan(f, top) || !xsdk_xsc_check_local_stores(l->x, l->m, f->locals, f->merged_local, merge_label(l->m, top)))
		return false;
	return xsdk_xsc_program_alloc(l->x, l->p, XSC_ALLOC_COLORS) && emit_output(f) && add_constants(f, constants) &&
		xsdk_xsc_program_finish(
			l->x, l->p, constants->items, constants->count, &out->container, &out->container_bytes) &&
		xsdk_xsc_literal_needs(l, out);
}

bool xsdk_xsc_lower_fragment(
	Xsc *x, const XscModule *m, const XsdkXscOptions *options, XscInterface *si, XscLowered *out)
{
	if (!xsdk_xsc_resolve_interface(x, m, si))
		return false;
	if (!si->pixel)
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "lower_fragment called on non-pixel module");
	const XscType *const types = xsdk_xsc_resolve_types(x, m);
	XscStores stores;
	XscProgram p;
	if (!types || !xsdk_xsc_store_map(x, m, &stores) || !xsdk_xsc_program_init(x, &p, true, options->pixel_registers))
		return false;
	p.optimize = options->optimize;
	// Temps run past r63 till the allocator packs them.
	p.whole_sources = true;
	p.group_fetches = options->group_fetches;
	int64_t highest, param_reg;
	if (!declare_inputs(x, m, types, &p, si, &highest, &param_reg))
		return false;
	Fragment f = { .stores = &stores };
	f.locals = xsdk_xsc_local_vars(x, m, 0, m->instr_count);
	f.gathered = xsdk_xsc_alloc(x, m->id_limit);
	if (!f.locals || !f.gathered ||
		!xsdk_xsc_lower_init(x, &f.l, m, si, &p, types, false, highest + 1, xsdk_xsc_literal_base(si)))
		return false;
	xsdk_xsc_fill_local_store(&f.l, &stores, f.locals);
	XscConstants constants = { 0 };
	return lower_body(&f, param_reg, &constants, out);
}
