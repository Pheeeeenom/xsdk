#include "xsc_lower.h"

// A pixel shader, lowered as lower.py's lower_fragment lowers it: samples and derivatives first, then one value, or an
// if/else of them as cnd selects, or a discard, to the color.

enum {
	// getGradients' .xzxz is ddx(src.xy) and .ywyw ddy(src.xy).
	GRADIENT_X = 0xE4,
	GRADIENT_Y = 0x39,
	// The parameter type xost's containers give a sampler.
	SAMPLER_PARAMETER = 0xC,
	// A pixel shader's depth goes to export 61, its x alone (xenia:src/xenia/gpu/ucode.h:1842,
	// dxbc_shader_translator.cc:1591-1594).
	EXPORT_DEPTH = 61,
	WRITE_X = 0x1,
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
	// What gl_FragDepth is given; 0 for none.
	uint32_t depth_value;
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

// A tfetch, its coordinate's work and any sample that coordinate or its LOD reads first.
static bool gather_sample(Fragment *f, uint32_t ssa, const XscInstr *ins)
{
	uint32_t inputs[XSC_SAMPLE_INPUTS];
	const uint32_t count = xsdk_xsc_sample_inputs(ins, inputs);
	for (uint32_t i = 0; i < count; i++) {
		if (!gather(f, inputs[i]))
			return false;
	}
	int64_t slot;
	return xsdk_xsc_emit_sample(&f->l, ssa, ins, &slot) && use_slot(f, slot);
}

// A hardware loop's samples: those its values on entry read, and those its passes read; one at coordinates a pass works
// out is refused as it's lowered.
static bool gather_loop(Fragment *f, const XscHardwareLoop *loop)
{
	const XscModule *const m = f->l.m;
	bool ok = !loop->break_condition || gather(f, loop->break_condition);
	for (size_t c = loop->carried_first; ok && c < loop->carried_first + loop->carried_count; c++)
		ok = gather(f, m->carried[c].init) && gather(f, m->carried[c].next);
	return ok;
}

static bool gather_value(Fragment *f, uint32_t ssa)
{
	const XscModule *const m = f->l.m;
	const XscInstr *const ins = xsdk_xsc_by_result(m, ssa);
	if (!ins)
		return true;
	const uint32_t op = ins->opcode;
	if (op == XSC_OP_LOOP_RESULT && f->l.loop_of[ssa] >= 0)
		return gather_loop(f, &m->loops[f->l.loop_of[ssa]]);
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

// gl_FragDepth's one store, outside the if/else, so every pixel exports what it holds.
static bool plan_depth(Fragment *f, const XscInstr *top)
{
	XscLower *const l = &f->l;
	const XscModule *const m = l->m;
	const uint32_t var = l->si->frag_depth->vid;
	uint32_t stores = 0;
	size_t at = 0;
	for (size_t i = 0; i < m->instr_count; i++) {
		if (m->instrs[i].opcode == SPV_STORE && xsdk_xsc_operand(&m->instrs[i], 0) == var) {
			stores++;
			at = i;
		}
	}
	if (stores != 1)
		return xsdk_xsc_lower_refuse(l, "gl_FragDepth stored %u times", (unsigned)stores);
	const XscInstr *const merge = top ? xsdk_xsc_by_result(m, merge_label(m, top)) : NULL;
	if (top && (!merge || (at > (size_t)(top - m->instrs) && at < (size_t)(merge - m->instrs))))
		return xsdk_xsc_lower_refuse(l, "gl_FragDepth stored inside an if/else");
	f->depth_value = f->stores->value[var];
	l->p->exports_depth = true;
	return gather(f, f->depth_value);
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
	const XscComparison *const c = xsdk_xsc_comparison(f->kill_cmp->opcode);
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
	const XscComparison *const c = xsdk_xsc_comparison(region->cmp->opcode);
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

static bool emit_depth(Fragment *f)
{
	if (!f->depth_value)
		return true;
	XscLower *const l = &f->l;
	const XscOrigin depth = xsdk_xsc_scalar_origin(l, f->depth_value);
	if (xsc_failed(l))
		return false;
	return xsdk_xsc_emit_copy(l, depth.o, EXPORT_DEPTH, true, false, origin_swizzle(depth), WRITE_X, false) != XSC_NONE;
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
	if (!plan(f, top) || (si->frag_depth && !plan_depth(f, top)) ||
		!xsdk_xsc_check_local_stores(l->x, l->m, f->locals, f->merged_local, merge_label(l->m, top)))
		return false;
	return xsdk_xsc_program_alloc(l->x, l->p, XSC_ALLOC_COLORS) && emit_output(f) && emit_depth(f) &&
		add_constants(f, constants) &&
		xsdk_xsc_program_finish(
			l->x, l->p, constants->items, constants->count, &out->container, &out->container_bytes) &&
		xsdk_xsc_literal_needs(l, out) && xsdk_xsc_loop_needs(l, out);
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
