#include <string.h>

#include "xsc_lower.h"

// The one loop shape lowered as a hardware loop, for (i = 0; i < u_count; i++) sum += k, as lower.py's
// lower_fragment_real_loop lowers it: the sum lives in a register and the host uploads the count as a loop constant.

enum {
	DEC_LOCATION = 30,
	STORAGE_UNIFORM_CONSTANT = 0,
	// The gallery's vertex shader exports a texcoord and a color, which the pixel shader must take though it reads
	// neither; the sum goes past them.
	ACC_REG = 2,
	STEP_REG = 0,
	INIT_REG = 1,
	DIFF_REG = ACC_REG + 1,
};

// A label's block: the instructions after it and its terminator, NULL past the function's end.
typedef struct {
	bool present;
	size_t start;
	size_t end;
	const XscInstr *term;
} Block;

typedef struct {
	Xsc *x;
	const XscModule *m;
	const XscInterface *si;
	Block *blocks;
	bool *locals;
	uint32_t iv_var;
	uint32_t acc_var;
	uint32_t merge_label;
	uint32_t loop_id;
	const char *bound_name;
	double step[4];
	double init[4];
	bool init_is_zero;
	// The uniform array the loop indexes, if it adds c[base + aL].
	bool array;
	int64_t array_base;
	int64_t array_regs;
	const char *array_name;
	bool has_break;
	bool break_condition;
	int64_t break_setp;
	bool break_negate;
	int64_t break_comp;
	double break_threshold;
} Loop;

static bool fail(Loop *lp, const char *message)
{
	return xsdk_xsc_fail(lp->x, XSDK_XSC_REFUSED, "real-loop v1: %s", message);
}

static const char *id_text(Loop *lp, uint32_t id)
{
	return id ? xsdk_xsc_format(lp->x, "%%%u", (unsigned)id) : "%None";
}

static bool fail_id(Loop *lp, uint32_t id, const char *what)
{
	const char *const text = id_text(lp, id);
	return text && xsdk_xsc_fail(lp->x, XSDK_XSC_REFUSED, "real-loop v1: %s is not %s", text, what);
}

static const Block *block(const Loop *lp, uint32_t label)
{
	static const Block none = { false, 0, 0, NULL };
	return label < lp->m->id_limit && lp->blocks[label].present ? &lp->blocks[label] : &none;
}

// The first instruction of a kind in a block, with operand 0 equal to target unless it's 0.
static const XscInstr *find(const Loop *lp, const Block *b, uint32_t opcode, uint32_t target)
{
	for (size_t i = b->start; i < b->end; i++) {
		const XscInstr *const ins = &lp->m->instrs[i];
		if (ins->opcode == opcode && (!target || xsdk_xsc_operand(ins, 0) == target))
			return ins;
	}
	return NULL;
}

static bool const_int(Loop *lp, uint32_t ssa, int64_t *value)
{
	const XscInstr *const ins = xsdk_xsc_by_result(lp->m, ssa);
	if (!ins || ins->opcode != SPV_CONSTANT)
		return fail_id(lp, ssa, "an int constant");
	const uint32_t bits = xsdk_xsc_operand(ins, 0);
	*value = bits >= 0x80000000u ? (int64_t)bits - 0x100000000LL : bits;
	return true;
}

static bool const_float(Loop *lp, uint32_t ssa, double *value)
{
	const XscInstr *const ins = xsdk_xsc_by_result(lp->m, ssa);
	if (!ins || ins->opcode != SPV_CONSTANT)
		return fail_id(lp, ssa, "a float constant");
	*value = xsdk_xsc_float_bits(xsdk_xsc_operand(ins, 0));
	return true;
}

static bool const_vec4(Loop *lp, uint32_t ssa, double value[4])
{
	const XscInstr *const ins = xsdk_xsc_by_result(lp->m, ssa);
	memset(value, 0, 4 * sizeof *value);
	if (ins && ins->opcode == SPV_CONSTANT)
		return const_float(lp, ssa, &value[0]);
	if (!ins || ins->opcode != SPV_CONSTANT_COMPOSITE)
		return fail_id(lp, ssa, "a constant vector");
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	for (uint32_t i = 0; i < count; i++) {
		double v;
		if (!const_float(lp, ops[i], &v))
			return false;
		if (i < 4)
			value[i] = v;
	}
	return true;
}

static uint32_t load_var(const Loop *lp, uint32_t ssa)
{
	const XscInstr *const ins = xsdk_xsc_by_result(lp->m, ssa);
	return ins && ins->opcode == SPV_LOAD ? xsdk_xsc_operand(ins, 0) : 0;
}

static bool is_local(const Loop *lp, uint32_t id)
{
	return id && id < lp->m->id_limit && lp->locals[id];
}

static size_t first_index(const XscModule *m, size_t from, size_t to, uint32_t opcode)
{
	for (size_t i = from; i < to; i++) {
		if (m->instrs[i].opcode == opcode)
			return i;
	}
	return to;
}

// The function's blocks by label: a block runs to a Branch, BranchConditional or Return.
static bool read_blocks(Loop *lp, uint32_t *entry_label)
{
	const XscModule *const m = lp->m;
	const size_t func = first_index(m, 0, m->instr_count, SPV_FUNCTION);
	const size_t end = first_index(m, 0, m->instr_count, SPV_FUNCTION_END);
	const size_t entry = first_index(m, func, end, SPV_LABEL);
	if (func == m->instr_count || end == m->instr_count || entry >= end)
		return xsdk_xsc_fail(lp->x, XSDK_XSC_MALFORMED, "no function body");
	*entry_label = m->instrs[entry].result_id;
	lp->blocks = xsdk_xsc_alloc(lp->x, (size_t)m->id_limit * sizeof *lp->blocks);
	lp->locals = xsdk_xsc_local_vars(lp->x, m, func, end);
	if (!lp->blocks || !lp->locals)
		return false;
	size_t i = entry;
	while (i < end) {
		if (m->instrs[i].opcode != SPV_LABEL) {
			i++;
			continue;
		}
		const uint32_t label = m->instrs[i].result_id;
		size_t j = i + 1;
		while (j < end && m->instrs[j].opcode != SPV_BRANCH && m->instrs[j].opcode != SPV_BRANCH_CONDITIONAL &&
			m->instrs[j].opcode != SPV_RETURN)
			j++;
		if (label < m->id_limit)
			lp->blocks[label] = (Block){ true, i + 1, j, j < end ? &m->instrs[j] : NULL };
		i = j + 1;
	}
	return true;
}

// The bound: a UniformConstant int whose Location names the loop constant.
static bool read_bound(Loop *lp, uint32_t bound_var)
{
	const XscInstr *const decl = xsdk_xsc_by_result(lp->m, bound_var);
	if (!decl || decl->opcode != SPV_VARIABLE || xsdk_xsc_operand(decl, 0) != STORAGE_UNIFORM_CONSTANT)
		return fail(lp, "loop bound must be a UniformConstant int");
	lp->loop_id = (uint32_t)xsdk_xsc_decoration_arg(xsdk_xsc_decoration(lp->m, bound_var, DEC_LOCATION), 0);
	const char *const fallback = xsdk_xsc_format(lp->x, "u_loop%u", (unsigned)lp->loop_id);
	lp->bound_name = fallback ? xsdk_xsc_name(lp->m, bound_var, fallback) : NULL;
	return lp->bound_name != NULL;
}

// The induction variable starts at zero and the continue block adds one.
static bool read_induction(Loop *lp, const Block *entry, uint32_t header_label, uint32_t cont_label)
{
	uint32_t init = 0;
	for (size_t i = entry->start; i < entry->end; i++) {
		const XscInstr *const ins = &lp->m->instrs[i];
		if (ins->opcode == SPV_STORE && xsdk_xsc_operand(ins, 0) == lp->iv_var)
			init = xsdk_xsc_operand(ins, 1);
	}
	int64_t start;
	if (!const_int(lp, init, &start))
		return false;
	if (start != 0)
		return fail(lp, "induction variable must start at zero");
	const Block *const cont = block(lp, cont_label);
	if (!cont->term || cont->term->opcode != SPV_BRANCH || xsdk_xsc_operand(cont->term, 0) != header_label)
		return fail(lp, "continue block must branch back to the header");
	const XscInstr *const store = find(lp, cont, SPV_STORE, lp->iv_var);
	if (!store)
		return fail(lp, "continue block must update the induction variable");
	const XscInstr *const inc = xsdk_xsc_by_result(lp->m, xsdk_xsc_operand(store, 1));
	if (!inc || inc->opcode != SPV_I_ADD)
		return fail(lp, "induction update must be i + 1");
	bool loads_iv = false;
	uint32_t first_const = 0;
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(inc, &count);
	for (uint32_t i = 0; i < count; i++) {
		const XscInstr *const c = xsdk_xsc_by_result(lp->m, ops[i]);
		loads_iv = loads_iv || load_var(lp, ops[i]) == lp->iv_var;
		if (!first_const && c && c->opcode == SPV_CONSTANT)
			first_const = ops[i];
	}
	int64_t step;
	if (!loads_iv || !first_const)
		return fail(lp, "induction update must be i + 1");
	if (!const_int(lp, first_const, &step))
		return false;
	return step == 1 || fail(lp, "induction update must be i + 1");
}

// The array a step that loads arr[i] adds from, c[base + aL].
static void read_array(Loop *lp, uint32_t step)
{
	const XscInstr *const ld = xsdk_xsc_by_result(lp->m, step);
	const XscInstr *const ac = ld && ld->opcode == SPV_LOAD ? xsdk_xsc_by_result(lp->m, xsdk_xsc_operand(ld, 0)) : NULL;
	if (!ac || ac->opcode != SPV_ACCESS_CHAIN)
		return;
	const XscInstr *const il = xsdk_xsc_by_result(lp->m, xsdk_xsc_operand(ac, 1));
	if (!il || il->opcode != SPV_LOAD || xsdk_xsc_operand(il, 0) != lp->iv_var)
		return;
	const XscRole *const role = xsdk_xsc_role(lp->si, xsdk_xsc_operand(ac, 0));
	if (role->kind != XSC_ROLE_ARRAY)
		return;
	lp->array = true;
	lp->array_base = role->a;
	lp->array_regs = role->b;
	const char *const name = xsdk_xsc_name(lp->m, xsdk_xsc_operand(ac, 0), "");
	lp->array_name = *name ? name : xsdk_xsc_format(lp->x, "u_arr%lld", (long long)role->a);
}

// The body adds a constant, or the array element the loop counter picks, to a local the entry block set.
static bool read_body(Loop *lp, const Block *entry, const Block *body)
{
	const XscInstr *const store = find(lp, body, SPV_STORE, 0);
	if (!store || !is_local(lp, xsdk_xsc_operand(store, 0)))
		return fail(lp, "loop body must store one local accumulator");
	lp->acc_var = xsdk_xsc_operand(store, 0);
	uint32_t init = 0;
	for (size_t i = entry->start; i < entry->end; i++) {
		const XscInstr *const ins = &lp->m->instrs[i];
		if (ins->opcode == SPV_STORE && xsdk_xsc_operand(ins, 0) == lp->acc_var)
			init = xsdk_xsc_operand(ins, 1);
	}
	if (!init)
		return fail(lp, "accumulator must be initialized before the loop");
	const XscInstr *const add = xsdk_xsc_by_result(lp->m, xsdk_xsc_operand(store, 1));
	if (!add || add->opcode != SPV_F_ADD)
		return fail(lp, "loop body must be sum += constant");
	int acc_index = -1;
	for (uint32_t i = 0; i < xsdk_xsc_operand_count(add); i++) {
		if (load_var(lp, xsdk_xsc_operand(add, i)) == lp->acc_var)
			acc_index = (int)i;
	}
	if (acc_index < 0)
		return fail(lp, "loop body must add to the accumulator");
	const uint32_t step = xsdk_xsc_operand(add, (uint32_t)(1 - acc_index));
	read_array(lp, step);
	if ((!lp->array && !const_vec4(lp, step, lp->step)) || !const_vec4(lp, init, lp->init))
		return false;
	lp->init_is_zero = lp->init[0] == 0.0 && lp->init[1] == 0.0 && lp->init[2] == 0.0 && lp->init[3] == 0.0;
	if (lp->array && !lp->init_is_zero)
		return fail(lp, "array-accumulating loop must start the accumulator at zero");
	return true;
}

// The component of the sum a Load of sum[c] reads, or -1.
static int64_t acc_component(const Loop *lp, uint32_t ssa)
{
	const XscInstr *const ld = xsdk_xsc_by_result(lp->m, ssa);
	const XscInstr *const ac = ld && ld->opcode == SPV_LOAD ? xsdk_xsc_by_result(lp->m, xsdk_xsc_operand(ld, 0)) : NULL;
	if (!ac || ac->opcode != SPV_ACCESS_CHAIN || xsdk_xsc_operand(ac, 0) != lp->acc_var)
		return -1;
	const XscInstr *const ci = xsdk_xsc_by_result(lp->m, xsdk_xsc_operand(ac, 1));
	return ci && ci->opcode == SPV_CONSTANT ? (int64_t)xsdk_xsc_operand(ci, 0) : -1;
}

static bool jumps_to(const Loop *lp, uint32_t label, uint32_t target)
{
	const Block *const b = block(lp, label);
	return b->start == b->end && b->term && b->term->opcode == SPV_BRANCH && xsdk_xsc_operand(b->term, 0) == target;
}

// The setp that tests sum.c CMP threshold, negated for < and <=, as acc - thr or thr - acc.
static void setp_for(uint32_t opcode, int64_t *setp, bool *negate)
{
	*negate = opcode == SPV_F_ORD_LESS_THAN || opcode == SPV_F_ORD_LESS_THAN_EQUAL;
	if (opcode == SPV_F_ORD_GREATER_THAN || opcode == SPV_F_ORD_LESS_THAN)
		*setp = XSC_SCALAR_SETP_GT;
	else if (opcode == SPV_F_ORD_GREATER_THAN_EQUAL || opcode == SPV_F_ORD_LESS_THAN_EQUAL)
		*setp = XSC_SCALAR_SETP_GE;
	else
		*setp = opcode == SPV_F_ORD_EQUAL ? XSC_SCALAR_SETP_EQ : XSC_SCALAR_SETP_NE;
}

static uint32_t flipped(uint32_t opcode)
{
	switch (opcode) {
	case SPV_F_ORD_GREATER_THAN:
		return SPV_F_ORD_LESS_THAN;
	case SPV_F_ORD_LESS_THAN:
		return SPV_F_ORD_GREATER_THAN;
	case SPV_F_ORD_GREATER_THAN_EQUAL:
		return SPV_F_ORD_LESS_THAN_EQUAL;
	case SPV_F_ORD_LESS_THAN_EQUAL:
		return SPV_F_ORD_GREATER_THAN_EQUAL;
	default:
		return opcode;
	}
}

// if (sum.c CMP constant) break; a setp on the sum and a predicated LOOP_END.
static bool read_break(Loop *lp, const XscInstr *term)
{
	if (jumps_to(lp, xsdk_xsc_operand(term, 1), lp->merge_label))
		lp->break_condition = true;
	else if (jumps_to(lp, xsdk_xsc_operand(term, 2), lp->merge_label))
		lp->break_condition = false;
	else
		return fail(lp, "loop body conditional must be a plain `break`");
	const XscInstr *const cmp = xsdk_xsc_by_result(lp->m, xsdk_xsc_operand(term, 0));
	const uint32_t op = cmp ? cmp->opcode : 0;
	if (op != SPV_F_ORD_EQUAL && op != SPV_F_ORD_NOT_EQUAL && op != SPV_F_ORD_LESS_THAN &&
		op != SPV_F_ORD_GREATER_THAN && op != SPV_F_ORD_LESS_THAN_EQUAL && op != SPV_F_ORD_GREATER_THAN_EQUAL)
		return fail(lp, "break condition must be a float comparison");
	int64_t comp = acc_component(lp, xsdk_xsc_operand(cmp, 0));
	uint32_t threshold = xsdk_xsc_operand(cmp, 1);
	uint32_t compare = op;
	if (comp < 0) {
		comp = acc_component(lp, xsdk_xsc_operand(cmp, 1));
		threshold = xsdk_xsc_operand(cmp, 0);
		compare = flipped(op);
	}
	if (comp < 0)
		return fail(lp, "break must compare the accumulator to a constant");
	setp_for(compare, &lp->break_setp, &lp->break_negate);
	lp->break_comp = comp;
	lp->has_break = true;
	return const_float(lp, threshold, &lp->break_threshold);
}

// The header branches to the condition, i < bound, which exits to the merge block or enters the body.
static bool read_condition(Loop *lp, const Block *header, uint32_t *cont_label, uint32_t *body_label)
{
	const XscInstr *const merge = find(lp, header, SPV_LOOP_MERGE, 0);
	if (!merge || !header->term || header->term->opcode != SPV_BRANCH)
		return fail(lp, "missing structured loop header");
	lp->merge_label = xsdk_xsc_operand(merge, 0);
	*cont_label = xsdk_xsc_operand(merge, 1);
	const Block *const cond = block(lp, xsdk_xsc_operand(header->term, 0));
	if (!cond->term || cond->term->opcode != SPV_BRANCH_CONDITIONAL)
		return fail(lp, "loop condition must branch conditionally");
	const XscInstr *const cmp = xsdk_xsc_by_result(lp->m, xsdk_xsc_operand(cond->term, 0));
	if (!cmp || cmp->opcode != SPV_S_LESS_THAN)
		return fail(lp, "loop condition must be signed i < bound");
	*body_label = xsdk_xsc_operand(cond->term, 1);
	if (xsdk_xsc_operand(cond->term, 2) != lp->merge_label)
		return fail(lp, "loop false branch must exit");
	lp->iv_var = load_var(lp, xsdk_xsc_operand(cmp, 0));
	const uint32_t bound_var = load_var(lp, xsdk_xsc_operand(cmp, 1));
	if (!is_local(lp, lp->iv_var) || !bound_var)
		return fail(lp, "condition must compare local induction var to a uniform");
	return read_bound(lp, bound_var);
}

static bool read_loop(Loop *lp)
{
	uint32_t entry_label = 0;
	if (!read_blocks(lp, &entry_label))
		return false;
	const Block *const entry = block(lp, entry_label);
	if (!entry->term || entry->term->opcode != SPV_BRANCH)
		return fail(lp, "entry must branch to a loop header");
	const uint32_t header_label = xsdk_xsc_operand(entry->term, 0);
	uint32_t cont_label = 0;
	uint32_t body_label = 0;
	if (!read_condition(lp, block(lp, header_label), &cont_label, &body_label) ||
		!read_induction(lp, entry, header_label, cont_label))
		return false;
	const Block *const body = block(lp, body_label);
	const bool plain_body =
		body->term && body->term->opcode == SPV_BRANCH && xsdk_xsc_operand(body->term, 0) == cont_label;
	const bool break_body = body->term && body->term->opcode == SPV_BRANCH_CONDITIONAL;
	if (!plain_body && !break_body)
		return fail(lp, "loop body must branch to continue (or conditionally break)");
	if (!read_body(lp, entry, body) || (break_body && !read_break(lp, body->term)))
		return false;
	const Block *const merge = block(lp, lp->merge_label);
	if (!merge->term || merge->term->opcode != SPV_RETURN)
		return fail(lp, "merge block must return");
	const XscInstr *const out = find(lp, merge, SPV_STORE, lp->si->color_out[0].var->vid);
	if (!out || load_var(lp, xsdk_xsc_operand(out, 1)) != lp->acc_var)
		return fail(lp, "merge block must export the accumulator");
	return true;
}

typedef struct {
	bool temp;
	int64_t reg;
	bool negate;
} Source;

static bool emit(Xsc *x, XscProgram *p, int64_t opcode, int64_t dst, Source a, Source b, bool export_)
{
	XscAlu f = xsdk_xsc_template_mov();
	f.vector_opcode = opcode;
	f.vector_dest = dst;
	f.is_export = export_;
	f.vector_write_mask = 0xF;
	f.vector_clamp = false;
	f.scalar_write_mask = 0;
	const Source s[2] = { a, b };
	for (int n = 0; n < 2; n++) {
		f.src_reg[n] = s[n].reg;
		f.src_is_temp[n] = s[n].temp;
		f.src_swizzle[n] = 0;
		f.src_negate[n] = s[n].negate;
	}
	return xsdk_xsc_program_alu(x, p, &f) != NULL;
}

static Source temp(int64_t reg, bool negate)
{
	return (Source){ true, reg, negate };
}

static Source constant(int64_t reg, bool negate)
{
	return (Source){ false, reg, negate };
}

// The register the threshold goes in, past the constants the loop uses.
static int64_t threshold_reg(const Loop *lp)
{
	if (lp->array)
		return lp->array_base + lp->array_regs;
	return lp->init_is_zero ? STEP_REG + 1 : INIT_REG + 1;
}

// p0 = sum.c CMP threshold; the loop breaks where p0 equals the break condition.
static bool emit_break(Xsc *x, XscProgram *p, const Loop *lp)
{
	const int64_t thr = threshold_reg(lp);
	const bool ok = lp->break_negate
		? emit(x, p, XSC_VECTOR_ADD, DIFF_REG, constant(thr, false), temp(ACC_REG, true), false)
		: emit(x, p, XSC_VECTOR_ADD, DIFF_REG, temp(ACC_REG, false), constant(thr, true), false);
	if (!ok)
		return false;
	XscAlu f = xsdk_xsc_template_mov();
	f.vector_opcode = XSC_VECTOR_MAX;
	f.vector_dest = 0;
	f.is_export = false;
	f.vector_write_mask = 0;
	f.vector_clamp = false;
	f.scalar_opcode = lp->break_setp;
	f.scalar_dest = 0;
	f.scalar_write_mask = 0;
	f.src_reg[2] = DIFF_REG;
	f.src_is_temp[2] = true;
	f.src_swizzle[2] = xsdk_xsc_bsw(lp->break_comp);
	return xsdk_xsc_program_alu(x, p, &f) && xsdk_xsc_program_end_loop(x, p, true, lp->break_condition);
}

// sum += c[base + aL], the element the loop counter picks.
static bool emit_array_add(Xsc *x, XscProgram *p, const Loop *lp)
{
	XscAlu f = xsdk_xsc_template_mov();
	f.vector_opcode = XSC_VECTOR_ADD;
	f.vector_dest = ACC_REG;
	f.is_export = false;
	f.vector_write_mask = 0xF;
	f.vector_clamp = false;
	f.scalar_write_mask = 0;
	f.src_reg[0] = ACC_REG;
	f.src_is_temp[0] = true;
	f.src_swizzle[0] = 0;
	f.src_negate[0] = false;
	f.src_reg[1] = lp->array_base;
	f.src_is_temp[1] = false;
	f.src_swizzle[1] = 0;
	f.src_negate[1] = false;
	f.src_const_rel[1] = true;
	return xsdk_xsc_program_alu(x, p, &f) != NULL;
}

static bool emit_loop(Xsc *x, XscProgram *p, const Loop *lp)
{
	if (!xsdk_xsc_program_input(x, p, 0, XSC_USAGE_TEXCOORD) || !xsdk_xsc_program_input(x, p, 1, XSC_USAGE_COLOR))
		return false;
	// A zero from c - c spends no literal.
	bool ok;
	if (lp->array)
		ok =
			emit(x, p, XSC_VECTOR_ADD, ACC_REG, constant(lp->array_base, false), constant(lp->array_base, true), false);
	else if (lp->init_is_zero)
		ok = emit(x, p, XSC_VECTOR_ADD, ACC_REG, constant(STEP_REG, false), constant(STEP_REG, true), false);
	else
		ok = emit(x, p, XSC_VECTOR_MAX, ACC_REG, constant(INIT_REG, false), constant(INIT_REG, false), false);
	if (!ok || !xsdk_xsc_program_begin_loop(x, p, lp->loop_id))
		return false;
	ok = lp->array ? emit_array_add(x, p, lp)
				   : emit(x, p, XSC_VECTOR_ADD, ACC_REG, temp(ACC_REG, false), constant(STEP_REG, false), false);
	if (!ok)
		return false;
	if (lp->has_break ? !emit_break(x, p, lp) : !xsdk_xsc_program_end_loop(x, p, false, false))
		return false;
	return xsdk_xsc_program_alloc(x, p, XSC_ALLOC_COLORS) &&
		emit(x, p, XSC_VECTOR_MAX, 0, temp(ACC_REG, false), temp(ACC_REG, false), true);
}

static bool add_literal(
	Xsc *x, XscConstants *constants, XscLowered *out, const char *name, int64_t reg, const double value[4])
{
	XsdkXscNeed need = { .kind = XSDK_XSC_LITERAL, .reg = (uint32_t)reg, .regs = 1 };
	memcpy(need.value, value, sizeof need.value);
	return xsdk_xsc_add_constant(x, constants, xsdk_xsc_vector_constant(name, reg, 1)) &&
		xsdk_xsc_add_need(x, out, need);
}

static bool add_constants(Xsc *x, const Loop *lp, XscConstants *constants, XscLowered *out)
{
	if (lp->array) {
		const XscConstant c = xsdk_xsc_vector_constant(lp->array_name, lp->array_base, lp->array_regs);
		const XsdkXscNeed need = { .kind = XSDK_XSC_ARRAY,
			.reg = (uint32_t)lp->array_base,
			.regs = (uint32_t)lp->array_regs,
			.name = lp->array_name };
		if (!xsdk_xsc_add_constant(x, constants, c) || !xsdk_xsc_add_need(x, out, need))
			return false;
	} else if (!add_literal(x, constants, out, "_lit0", STEP_REG, lp->step) ||
		(!lp->init_is_zero && !add_literal(x, constants, out, "_lit1", INIT_REG, lp->init))) {
		return false;
	}
	const double threshold[4] = { lp->break_threshold, lp->break_threshold, lp->break_threshold, lp->break_threshold };
	if (lp->has_break && !add_literal(x, constants, out, "_thr", threshold_reg(lp), threshold))
		return false;
	const XsdkXscNeed loop = { .kind = XSDK_XSC_LOOP, .reg = lp->loop_id, .name = lp->bound_name };
	return xsdk_xsc_add_need(x, out, loop);
}

bool xsdk_xsc_lower_real_loop(Xsc *x, const XscModule *m, uint32_t registers, XscInterface *si, XscLowered *out)
{
	if (!xsdk_xsc_resolve_interface(x, m, si))
		return false;
	if (!si->pixel)
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "real-loop lowering is pixel-only");
	if (si->color_out_count != 1)
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "real-loop path expects one color output");
	if (si->frag_coord)
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "gl_FragCoord in a hardware loop (v1)");
	Loop lp = { .x = x, .m = m, .si = si };
	XscProgram p;
	XscConstants constants = { 0 };
	if (!read_loop(&lp) || !xsdk_xsc_program_init(x, &p, true, registers) || !emit_loop(x, &p, &lp) ||
		!add_constants(x, &lp, &constants, out))
		return false;
	return xsdk_xsc_program_finish(x, &p, constants.items, constants.count, &out->container, &out->container_bytes);
}
