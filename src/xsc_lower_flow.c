#include "xsc_lower.h"

// Control flow the unroller left as values: the selects where an if/else's arms meet (xsc_unroll.c), each a cnd on the
// comparison it branched on.

// How a comparison selects (cnd), and kills where it holds and where it doesn't (lower.py _CMP, _KILL_TRUE,
// _KILL_FALSE).
static const XscComparison comparisons[] = {
	{ SPV_F_ORD_GREATER_THAN, XSC_VECTOR_CND_GT, false, false, XSC_VECTOR_KILL_GT, false, XSC_VECTOR_KILL_GE, true },
	{ SPV_F_ORD_LESS_THAN, XSC_VECTOR_CND_GT, true, false, XSC_VECTOR_KILL_GT, true, XSC_VECTOR_KILL_GE, false },
	{ SPV_F_ORD_GREATER_THAN_EQUAL, XSC_VECTOR_CND_GE, false, false, XSC_VECTOR_KILL_GE, false, XSC_VECTOR_KILL_GT,
		true },
	{ SPV_F_ORD_LESS_THAN_EQUAL, XSC_VECTOR_CND_GE, true, false, XSC_VECTOR_KILL_GE, true, XSC_VECTOR_KILL_GT, false },
	{ SPV_F_ORD_EQUAL, XSC_VECTOR_CND_EQ, false, false, XSC_VECTOR_KILL_EQ, false, XSC_VECTOR_KILL_NE, false },
	{ SPV_F_ORD_NOT_EQUAL, XSC_VECTOR_CND_EQ, false, true, XSC_VECTOR_KILL_NE, false, XSC_VECTOR_KILL_EQ, false },
};

const XscComparison *xsdk_xsc_comparison(uint32_t opcode)
{
	for (size_t i = 0; i < sizeof comparisons / sizeof comparisons[0]; i++) {
		if (comparisons[i].opcode == opcode)
			return &comparisons[i];
	}
	return NULL;
}

bool xsdk_xsc_is_merge(const XscLower *l, uint32_t ssa)
{
	return ssa < l->m->id_limit && l->merge_branch[ssa] >= 0;
}

static int64_t origin_swizzle(XscOrigin o)
{
	return o.comp == XSC_BROADCAST ? 0 : xsdk_xsc_bsw(o.comp);
}

// What an arm gives the select: a bool as 1 or 0, an integer as the float that holds it, or the value.
static XscOperand arm_value(XscLower *l, uint32_t ssa, XscTypeKind kind)
{
	if (kind == XSC_TYPE_BOOL)
		return xsdk_xsc_bool01(l, ssa);
	if (kind == XSC_TYPE_INT)
		return xsdk_xsc_int_operand(l, ssa);
	return xsdk_xsc_lower_operand(l, ssa);
}

// The register a cnd tests, and the cnd: a - b for a comparison it takes, as the fragment lowering's if/else does;
// otherwise the bool as 1 or 0, which cndgt takes as it is. swap says the arms trade places.
static bool work_out_test(XscLower *l, uint32_t cond, XscOperand *test, int64_t *opcode, bool *swap)
{
	const XscInstr *const cmp = xsdk_xsc_lower_result(l, cond);
	if (!cmp)
		return false;
	const XscComparison *const c = xsdk_xsc_comparison(cmp->opcode);
	*swap = false;
	if (!c) {
		*test = xsdk_xsc_bool01(l, cond);
		*opcode = XSC_VECTOR_CND_GT;
		return !xsc_failed(l);
	}
	XscOrigin a = xsdk_xsc_scalar_origin(l, xsdk_xsc_operand(cmp, 0));
	if (xsc_failed(l))
		return false;
	XscOrigin b = xsdk_xsc_scalar_origin(l, xsdk_xsc_operand(cmp, 1));
	if (xsc_failed(l))
		return false;
	if (c->cnd_swap_operands) {
		const XscOrigin t = a;
		a = b;
		b = t;
	}
	const int64_t d = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_src(a.o, false, origin_swizzle(a)), xsc_src(b.o, true, origin_swizzle(b)), d,
			false, 0xF))
		return false;
	*test = xsc_reg(d);
	*opcode = c->cnd;
	*swap = c->cnd_swap_arms;
	return true;
}

// A branch's selects share its test.
static bool merge_test(XscLower *l, uint32_t branch, XscOperand *test, int64_t *opcode, bool *swap)
{
	if (l->branch_test[branch].kind == XSC_OPERAND_NONE &&
		!work_out_test(l, l->m->branches[branch].condition, &l->branch_test[branch], &l->branch_cnd[branch],
			&l->branch_swap[branch]))
		return false;
	*test = l->branch_test[branch];
	*opcode = l->branch_cnd[branch];
	*swap = l->branch_swap[branch];
	return true;
}

bool xsdk_xsc_emit_merge(XscLower *l, uint32_t ssa, int64_t dst, bool export_)
{
	const XscInstr *const ins = xsdk_xsc_lower_result(l, ssa);
	if (!ins)
		return false;
	const XscType *const type = xsdk_xsc_result_type(l, ssa);
	const XscTypeKind kind = type ? type->kind : XSC_TYPE_NONE;
	if (kind == XSC_TYPE_VECTOR) {
		const XscType *const comp = xsdk_xsc_type(l->m, l->types, type->comp);
		if (comp && comp->kind == XSC_TYPE_INT)
			return xsdk_xsc_lower_refuse(l, "an integer vector chosen by an if/else (v1)");
	} else if (kind != XSC_TYPE_FLOAT && kind != XSC_TYPE_BOOL && kind != XSC_TYPE_INT) {
		return xsdk_xsc_lower_refuse(l, "a %s chosen by an if/else (v1)", xsdk_xsc_type_kind_name(kind));
	}
	XscOperand test;
	int64_t opcode;
	bool swap;
	if (!merge_test(l, (uint32_t)l->merge_branch[ssa], &test, &opcode, &swap))
		return false;
	const XscOperand tv = arm_value(l, xsdk_xsc_operand(ins, 1), kind);
	if (xsc_failed(l))
		return false;
	XscOperand fv = arm_value(l, xsdk_xsc_operand(ins, 2), kind);
	if (xsc_failed(l))
		return false;
	// cnd takes one constant register.
	if (tv.kind == XSC_OPERAND_CONST && fv.kind == XSC_OPERAND_CONST) {
		fv = xsdk_xsc_to_reg(l, fv);
		if (xsc_failed(l))
			return false;
	}
	const XscOperand first = swap ? fv : tv;
	const XscOperand second = swap ? tv : fv;
	return xsc_emit3(l, opcode, xsc_plain(test), xsc_plain(first), xsc_plain(second), dst, export_, 0xF);
}

XscOperand xsdk_xsc_merge_operand(XscLower *l, uint32_t ssa)
{
	const int64_t r = xsdk_xsc_fresh(l);
	return xsdk_xsc_emit_merge(l, ssa, r, false) ? xsc_reg(r) : (XscOperand){ 0 };
}
