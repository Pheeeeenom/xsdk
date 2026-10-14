#include "xsc_lower.h"

// GLSL.std.450 instructions, lowered as xenos-toolchain/xsc/lower.py's ValueLowerer.extinst lowers them.

enum {
	GLSL_ROUND = 1,
	GLSL_ROUND_EVEN = 2,
	GLSL_TRUNC = 3,
	GLSL_F_ABS = 4,
	GLSL_F_SIGN = 6,
	GLSL_FLOOR = 8,
	GLSL_CEIL = 9,
	GLSL_FRACT = 10,
	GLSL_RADIANS = 11,
	GLSL_DEGREES = 12,
	GLSL_SIN = 13,
	GLSL_COS = 14,
	GLSL_TAN = 15,
	GLSL_ASIN = 16,
	GLSL_ACOS = 17,
	GLSL_ATAN = 18,
	GLSL_POW = 26,
	GLSL_EXP = 27,
	GLSL_LOG = 28,
	GLSL_EXP2 = 29,
	GLSL_LOG2 = 30,
	GLSL_SQRT = 31,
	GLSL_INVERSE_SQRT = 32,
	GLSL_DETERMINANT = 33,
	GLSL_F_MIN = 37,
	GLSL_F_MAX = 40,
	GLSL_F_CLAMP = 43,
	GLSL_F_MIX = 46,
	GLSL_STEP = 48,
	GLSL_SMOOTH_STEP = 49,
	GLSL_LENGTH = 66,
	GLSL_DISTANCE = 67,
	GLSL_CROSS = 68,
	GLSL_NORMALIZE = 69,
	GLSL_REFLECT = 71,
	GLSL_REFRACT = 72,
};

static const double HALF_PI = 1.5707963;

static uint32_t arg(const XscInstr *ins, uint32_t i)
{
	return xsdk_xsc_operand(ins, 2 + i);
}

static bool arg_operand(XscLower *l, const XscInstr *ins, uint32_t i, XscOperand *o)
{
	*o = xsdk_xsc_lower_operand(l, arg(ins, i));
	return !xsc_failed(l);
}

static bool arg_register(XscLower *l, const XscInstr *ins, uint32_t i, XscOperand *o)
{
	*o = xsdk_xsc_to_reg(l, xsdk_xsc_lower_operand(l, arg(ins, i)));
	return !xsc_failed(l);
}

static bool splat(XscLower *l, double value, XscOperand *o)
{
	const int64_t reg = xsdk_xsc_literal_splat(l, value);
	*o = xsc_const(reg);
	return reg != XSC_NONE;
}

static int64_t done(bool ok, int64_t dst)
{
	return ok ? dst : XSC_NONE;
}

// A scalar op's result in dst, through a temp when it's exported: the scalar unit doesn't export.
static int64_t scalar_result(XscLower *l, int64_t sop, XscOperand src, int64_t comp, int64_t dst, bool export_)
{
	if (!export_)
		return xsdk_xsc_emit_scalar(l, sop, dst, src, comp, 0xF);
	const int64_t r = xsdk_xsc_emit_scalar(l, sop, xsdk_xsc_fresh(l), src, comp, 0xF);
	return done(r != XSC_NONE && xsc_emit2(l, XSC_VECTOR_MAX, xsc_temp(r), xsc_temp(r), dst, true, 0xF), dst);
}

// A scalar as an operand, the swizzle that broadcasts it and the component the scalar unit reads.
typedef struct {
	XscOperand o;
	int64_t swizzle;
	int64_t comp;
} ScalarSource;

static bool scalar_source(XscLower *l, uint32_t ssa, ScalarSource *s)
{
	const XscOrigin origin = xsdk_xsc_scalar_origin(l, ssa);
	if (xsc_failed(l))
		return false;
	const bool broadcast = origin.comp == XSC_BROADCAST;
	*s = (ScalarSource){ origin.o, broadcast ? 0 : xsdk_xsc_bsw(origin.comp), broadcast ? 3 : origin.comp };
	return true;
}

static int64_t broadcast(XscLower *l, uint32_t ssa)
{
	ScalarSource s;
	if (!scalar_source(l, ssa, &s))
		return XSC_NONE;
	const int64_t r = xsdk_xsc_fresh(l);
	const XscSrc src = xsc_src(s.o, false, s.swizzle);
	return done(xsc_emit2(l, XSC_VECTOR_MAX, src, src, r, false, 0xF), r);
}

static int64_t horner(XscLower *l, int64_t x_reg, const double *coeffs, size_t count)
{
	XscOperand first;
	if (!splat(l, coeffs[0], &first))
		return XSC_NONE;
	const XscOperand p = xsdk_xsc_to_reg(l, first);
	if (xsc_failed(l))
		return XSC_NONE;
	for (size_t k = 1; k < count; k++) {
		XscOperand ck;
		if (!splat(l, coeffs[k], &ck) ||
			!xsc_emit3(l, XSC_VECTOR_MAD, xsc_temp(x_reg), xsc_plain(p), xsc_plain(ck), p.value, false, 0xF))
			return XSC_NONE;
	}
	return p.value;
}

// sign(x) * (pi/2 - sqrt(1 - |x|) * p(|x|)).
static int64_t asin_reg(XscLower *l, uint32_t ssa)
{
	static const double coeffs[] = { -0.0187293, 0.0742610, -0.2121144, 1.5707288 };
	const int64_t xr = broadcast(l, ssa);
	if (xr == XSC_NONE)
		return XSC_NONE;
	const int64_t ax = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_MAX, xsc_temp(xr), xsc_negated(xsc_reg(xr)), ax, false, 0xF))
		return XSC_NONE;
	const int64_t p = horner(l, ax, coeffs, sizeof coeffs / sizeof coeffs[0]);
	XscOperand one;
	if (p == XSC_NONE || !splat(l, 1.0, &one))
		return XSC_NONE;
	const int64_t om = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_plain(one), xsc_negated(xsc_reg(ax)), om, false, 0xF))
		return XSC_NONE;
	const int64_t s = xsdk_xsc_emit_scalar(l, XSC_SCALAR_SQRT, xsdk_xsc_fresh(l), xsc_reg(om), 3, 0xF);
	XscOperand half_pi;
	if (s == XSC_NONE || !splat(l, HALF_PI, &half_pi))
		return XSC_NONE;
	const int64_t inner = xsdk_xsc_fresh(l);
	if (!xsc_emit3(l, XSC_VECTOR_MAD, xsc_negated(xsc_reg(s)), xsc_temp(p), xsc_plain(half_pi), inner, false, 0xF))
		return XSC_NONE;
	const int64_t gt = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_SGT, xsc_temp(xr), xsc_negated(xsc_reg(xr)), gt, false, 0xF))
		return XSC_NONE;
	const int64_t lt = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_SGT, xsc_negated(xsc_reg(xr)), xsc_temp(xr), lt, false, 0xF))
		return XSC_NONE;
	const int64_t sgn = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_temp(gt), xsc_negated(xsc_reg(lt)), sgn, false, 0xF))
		return XSC_NONE;
	const int64_t out = xsdk_xsc_fresh(l);
	return done(xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(sgn), xsc_temp(inner), out, false, 0xF), out);
}

// x * p(x^2).
static int64_t atan_reg(XscLower *l, uint32_t ssa)
{
	static const double coeffs[] = { 0.0208351, -0.0851330, 0.1801410, -0.3302995, 0.9998660 };
	const int64_t xr = broadcast(l, ssa);
	if (xr == XSC_NONE)
		return XSC_NONE;
	const int64_t x2 = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(xr), xsc_temp(xr), x2, false, 0xF))
		return XSC_NONE;
	const int64_t p = horner(l, x2, coeffs, sizeof coeffs / sizeof coeffs[0]);
	if (p == XSC_NONE)
		return XSC_NONE;
	const int64_t out = xsdk_xsc_fresh(l);
	return done(xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(xr), xsc_temp(p), out, false, 0xF), out);
}

// x + (y - x) * a.
static int64_t mix(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand ox, oy, oa;
	if (!arg_operand(l, ins, 0, &ox) || !arg_operand(l, ins, 1, &oy) || !arg_operand(l, ins, 2, &oa))
		return XSC_NONE;
	const int64_t tmp = xsdk_xsc_fresh(l);
	return done(xsc_emit2(l, XSC_VECTOR_ADD, xsc_plain(oy), xsc_negated(ox), tmp, false, 0xF) &&
			xsc_emit3(l, XSC_VECTOR_MAD, xsc_temp(tmp), xsc_plain(oa), xsc_plain(ox), dst, export_, 0xF),
		dst);
}

static int64_t binary(XscLower *l, const XscInstr *ins, int64_t opcode, int64_t dst, bool export_)
{
	XscOperand a, b;
	if (!arg_operand(l, ins, 0, &a) || !arg_operand(l, ins, 1, &b))
		return XSC_NONE;
	return done(xsc_emit2(l, opcode, xsc_plain(a), xsc_plain(b), dst, export_, 0xF), dst);
}

static int64_t unary(XscLower *l, const XscInstr *ins, int64_t opcode, bool negate_second, int64_t dst, bool export_)
{
	XscOperand ox;
	if (!arg_operand(l, ins, 0, &ox))
		return XSC_NONE;
	return done(xsc_emit2(l, opcode, xsc_plain(ox), xsc_src(ox, negate_second, 0), dst, export_, 0xF), dst);
}

static int64_t normalize(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand ov;
	if (!arg_operand(l, ins, 0, &ov))
		return XSC_NONE;
	const int64_t d = xsdk_xsc_dotself(l, ov, xsdk_xsc_vec_size(l, arg(ins, 0)));
	if (d == XSC_NONE)
		return XSC_NONE;
	const int64_t s = xsdk_xsc_emit_scalar(l, XSC_SCALAR_RSQ, xsdk_xsc_fresh(l), xsc_reg(d), 3, 0xF);
	return done(s != XSC_NONE && xsc_emit2(l, XSC_VECTOR_MUL, xsc_plain(ov), xsc_temp(s), dst, export_, 0xF), dst);
}

static int64_t length(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand ov;
	if (!arg_operand(l, ins, 0, &ov))
		return XSC_NONE;
	const int64_t d = xsdk_xsc_dotself(l, ov, xsdk_xsc_vec_size(l, arg(ins, 0)));
	return d == XSC_NONE ? XSC_NONE : scalar_result(l, XSC_SCALAR_SQRT, xsc_reg(d), 3, dst, export_);
}

static int64_t distance(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand oa, ob;
	if (!arg_operand(l, ins, 0, &oa) || !arg_operand(l, ins, 1, &ob))
		return XSC_NONE;
	const int64_t diff = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_plain(oa), xsc_negated(ob), diff, false, 0xF))
		return XSC_NONE;
	const int64_t d = xsdk_xsc_dotself(l, xsc_reg(diff), xsdk_xsc_vec_size(l, arg(ins, 0)));
	return d == XSC_NONE ? XSC_NONE : scalar_result(l, XSC_SCALAR_SQRT, xsc_reg(d), 3, dst, export_);
}

static int64_t scalar_op(XscLower *l, const XscInstr *ins, int64_t sop, int64_t dst, bool export_)
{
	XscOperand ox;
	if (!arg_operand(l, ins, 0, &ox))
		return XSC_NONE;
	return scalar_result(l, sop, ox, 3, dst, export_);
}

// exp(x) = exp2(x * log2(e)).
static int64_t natural_exp(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	ScalarSource s;
	XscOperand ck;
	if (!scalar_source(l, arg(ins, 0), &s) || !splat(l, 1.4426950408, &ck))
		return XSC_NONE;
	const int64_t mm = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_MUL, xsc_src(s.o, false, s.swizzle), xsc_plain(ck), mm, false, 0xF))
		return XSC_NONE;
	return scalar_result(l, XSC_SCALAR_EXP, xsc_reg(mm), 3, dst, export_);
}

// log(x) = log2(x) * ln(2).
static int64_t natural_log(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	ScalarSource s;
	if (!scalar_source(l, arg(ins, 0), &s))
		return XSC_NONE;
	const int64_t lg = xsdk_xsc_emit_scalar(l, XSC_SCALAR_LOG, xsdk_xsc_fresh(l), s.o, s.comp, 0xF);
	XscOperand ck;
	if (lg == XSC_NONE || !splat(l, 0.6931471805, &ck))
		return XSC_NONE;
	return done(xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(lg), xsc_plain(ck), dst, export_, 0xF), dst);
}

static int64_t sin_cos(XscLower *l, const XscInstr *ins, int64_t sop, int64_t dst, bool export_)
{
	ScalarSource s;
	if (!scalar_source(l, arg(ins, 0), &s))
		return XSC_NONE;
	return scalar_result(l, sop, s.o, s.comp, dst, export_);
}

// i - 2 * dot(n, i) * n.
static int64_t reflect(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand oi, on;
	if (!arg_operand(l, ins, 0, &oi) || !arg_operand(l, ins, 1, &on))
		return XSC_NONE;
	const int64_t n = xsdk_xsc_vec_size(l, arg(ins, 0));
	int64_t d;
	if (arg(ins, 0) == arg(ins, 1)) {
		d = xsdk_xsc_dotself(l, on, n);
	} else {
		d = xsdk_xsc_fresh(l);
		const int64_t dp = n == 3 ? XSC_VECTOR_DP3 : XSC_VECTOR_DP4;
		d = done(xsc_emit2(l, dp, xsc_plain(on), xsc_plain(oi), d, false, 0xF), d);
	}
	if (d == XSC_NONE)
		return XSC_NONE;
	const int64_t d2 = xsdk_xsc_fresh(l);
	return done(xsc_emit2(l, XSC_VECTOR_ADD, xsc_temp(d), xsc_temp(d), d2, false, 0xF) &&
			xsc_emit3(l, XSC_VECTOR_MAD, xsc_negated(xsc_reg(d2)), xsc_plain(on), xsc_plain(oi), dst, export_, 0xF),
		dst);
}

// exp2(log2(x) * y), a component at a time.
static int64_t power(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand ox, oy;
	if (!arg_operand(l, ins, 0, &ox) || !arg_operand(l, ins, 1, &oy))
		return XSC_NONE;
	const int64_t size = xsdk_xsc_vec_size(l, arg(ins, 0));
	const int64_t n = size > 1 ? size : 1;
	for (int64_t i = 0; i < n; i++) {
		const int64_t li = xsdk_xsc_emit_scalar(l, XSC_SCALAR_LOG, xsdk_xsc_fresh(l), ox, i, 0xF);
		if (li == XSC_NONE)
			return XSC_NONE;
		const int64_t mi = xsdk_xsc_fresh(l);
		if (!xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(li), xsc_src(oy, false, xsdk_xsc_bsw(0)), mi, false, 0xF))
			return XSC_NONE;
		const int64_t ri = xsdk_xsc_emit_scalar(l, XSC_SCALAR_EXP, xsdk_xsc_fresh(l), xsc_reg(mi), 3, 0xF);
		if (ri == XSC_NONE || !xsc_emit2(l, XSC_VECTOR_MAX, xsc_temp(ri), xsc_temp(ri), dst, export_, (int64_t)1 << i))
			return XSC_NONE;
	}
	return dst;
}

// (x > 0) - (x < 0).
static int64_t sign(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand ox;
	if (!arg_operand(l, ins, 0, &ox))
		return XSC_NONE;
	if (ox.kind == XSC_OPERAND_CONST) {
		const int64_t xr = xsdk_xsc_fresh(l);
		if (!xsc_emit2(l, XSC_VECTOR_MAX, xsc_plain(ox), xsc_plain(ox), xr, false, 0xF))
			return XSC_NONE;
		ox = xsc_reg(xr);
	}
	const int64_t gt = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_SGT, xsc_plain(ox), xsc_negated(ox), gt, false, 0xF))
		return XSC_NONE;
	const int64_t lt = xsdk_xsc_fresh(l);
	return done(xsc_emit2(l, XSC_VECTOR_SGT, xsc_negated(ox), xsc_plain(ox), lt, false, 0xF) &&
			xsc_emit2(l, XSC_VECTOR_ADD, xsc_temp(gt), xsc_negated(xsc_reg(lt)), dst, export_, 0xF),
		dst);
}

static int64_t tangent(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	ScalarSource s;
	if (!scalar_source(l, arg(ins, 0), &s))
		return XSC_NONE;
	const int64_t si = xsdk_xsc_emit_scalar(l, XSC_SCALAR_SIN, xsdk_xsc_fresh(l), s.o, s.comp, 0xF);
	if (si == XSC_NONE)
		return XSC_NONE;
	const int64_t co = xsdk_xsc_emit_scalar(l, XSC_SCALAR_COS, xsdk_xsc_fresh(l), s.o, s.comp, 0xF);
	if (co == XSC_NONE)
		return XSC_NONE;
	const int64_t rc = xsdk_xsc_emit_scalar(l, XSC_SCALAR_RCP, xsdk_xsc_fresh(l), xsc_reg(co), 3, 0xF);
	return done(rc != XSC_NONE && xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(si), xsc_temp(rc), dst, export_, 0xF), dst);
}

// -floor(-x).
static int64_t ceiling(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand ox;
	if (!arg_operand(l, ins, 0, &ox))
		return XSC_NONE;
	const int64_t tf = xsdk_xsc_fresh(l);
	const XscSrc negated = xsc_negated(xsc_reg(tf));
	return done(xsc_emit2(l, XSC_VECTOR_FLOOR, xsc_negated(ox), xsc_negated(ox), tf, false, 0xF) &&
			xsc_emit2(l, XSC_VECTOR_MAX, negated, negated, dst, export_, 0xF),
		dst);
}

// floor(x + 0.5).
static int64_t round_half_up(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand ox, half;
	if (!arg_register(l, ins, 0, &ox) || !splat(l, 0.5, &half))
		return XSC_NONE;
	const int64_t s = xsdk_xsc_fresh(l);
	return done(xsc_emit2(l, XSC_VECTOR_ADD, xsc_plain(ox), xsc_plain(half), s, false, 0xF) &&
			xsc_emit2(l, XSC_VECTOR_FLOOR, xsc_temp(s), xsc_temp(s), dst, export_, 0xF),
		dst);
}

static int64_t scale(XscLower *l, const XscInstr *ins, double k, int64_t dst, bool export_)
{
	XscOperand ox, ck;
	if (!arg_register(l, ins, 0, &ox) || !splat(l, k, &ck))
		return XSC_NONE;
	return done(xsc_emit2(l, XSC_VECTOR_MUL, xsc_plain(ox), xsc_plain(ck), dst, export_, 0xF), dst);
}

static int64_t step(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand oe, ox;
	if (!arg_operand(l, ins, 0, &oe) || !arg_register(l, ins, 1, &ox))
		return XSC_NONE;
	return done(xsc_emit2(l, XSC_VECTOR_SGE, xsc_plain(ox), xsc_plain(oe), dst, export_, 0xF), dst);
}

// t * t * (3 - 2t), t = saturate((x - e0) / (e1 - e0)).
static int64_t smooth_step(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand oe0, oe1, ox;
	if (!arg_operand(l, ins, 0, &oe0) || !arg_operand(l, ins, 1, &oe1) || !arg_operand(l, ins, 2, &ox))
		return XSC_NONE;
	const XscOperand e0r = xsdk_xsc_to_reg(l, oe0);
	if (xsc_failed(l))
		return XSC_NONE;
	const int64_t size = xsdk_xsc_vec_size(l, arg(ins, 2));
	const int64_t num = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_plain(ox), xsc_negated(e0r), num, false, 0xF))
		return XSC_NONE;
	const int64_t den = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_plain(oe1), xsc_negated(e0r), den, false, 0xF))
		return XSC_NONE;
	const int64_t rden = xsdk_xsc_recip(l, xsc_reg(den), size > 1 ? size : 1);
	if (rden == XSC_NONE)
		return XSC_NONE;
	const int64_t tt = xsdk_xsc_fresh(l);
	const XscSrc ratio[2] = { xsc_temp(num), xsc_temp(rden) };
	if (!xsdk_xsc_emit(l, XSC_VECTOR_MUL, ratio, 2, tt, false, 0xF, true))
		return XSC_NONE;
	const int64_t two_t = xsdk_xsc_fresh(l);
	XscOperand three;
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_temp(tt), xsc_temp(tt), two_t, false, 0xF) || !splat(l, 3.0, &three))
		return XSC_NONE;
	const int64_t h = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_plain(three), xsc_negated(xsc_reg(two_t)), h, false, 0xF))
		return XSC_NONE;
	const int64_t t2 = xsdk_xsc_fresh(l);
	return done(xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(tt), xsc_temp(tt), t2, false, 0xF) &&
			xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(t2), xsc_temp(h), dst, export_, 0xF),
		dst);
}

// a.yzx * b.zxy - a.zxy * b.yzx, with three-component swizzles that leave w alone.
static int64_t cross(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	static const int64_t yzx_idxs[] = { 1, 2, 0 };
	static const int64_t zxy_idxs[] = { 2, 0, 1 };
	XscOperand oa, ob;
	if (!arg_operand(l, ins, 0, &oa) || !arg_operand(l, ins, 1, &ob))
		return XSC_NONE;
	const int64_t yzx = xsdk_xsc_swz(yzx_idxs, 3);
	const int64_t zxy = xsdk_xsc_swz(zxy_idxs, 3);
	const int64_t tc = xsdk_xsc_fresh(l);
	return done(xsc_emit2(l, XSC_VECTOR_MUL, xsc_src(oa, false, zxy), xsc_src(ob, false, yzx), tc, false, 0xF) &&
			xsc_emit3(l, XSC_VECTOR_MAD, xsc_src(oa, false, yzx), xsc_src(ob, false, zxy), xsc_negated(xsc_reg(tc)),
				dst, export_, 0xF),
		dst);
}

static int64_t copied(XscLower *l, int64_t r, int64_t dst, bool export_)
{
	return done(r != XSC_NONE && xsc_emit2(l, XSC_VECTOR_MAX, xsc_temp(r), xsc_temp(r), dst, export_, 0xF), dst);
}

// k = 1 - eta^2 (1 - dot(n, i)^2); k < 0 ? 0 : eta i - (eta dot(n, i) + sqrt(k)) n.
static int64_t refract(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	XscOperand oi, on, one;
	if (!arg_operand(l, ins, 0, &oi) || !arg_operand(l, ins, 1, &on))
		return XSC_NONE;
	const int64_t n = xsdk_xsc_vec_size(l, arg(ins, 0));
	const int64_t eta_reg = broadcast(l, arg(ins, 2));
	if (eta_reg == XSC_NONE || !splat(l, 1.0, &one))
		return XSC_NONE;
	const XscSrc eta = xsc_temp(eta_reg);
	const int64_t d = xsdk_xsc_fresh(l);
	const int64_t dp = n == 3 ? XSC_VECTOR_DP3 : XSC_VECTOR_DP4;
	if (!xsc_emit2(l, dp, xsc_plain(on), xsc_plain(oi), d, false, 0xF))
		return XSC_NONE;
	const int64_t d2 = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(d), xsc_temp(d), d2, false, 0xF))
		return XSC_NONE;
	const int64_t om = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_plain(one), xsc_negated(xsc_reg(d2)), om, false, 0xF))
		return XSC_NONE;
	const int64_t e2 = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_MUL, eta, eta, e2, false, 0xF))
		return XSC_NONE;
	const int64_t t = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(e2), xsc_temp(om), t, false, 0xF))
		return XSC_NONE;
	const int64_t k = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_ADD, xsc_plain(one), xsc_negated(xsc_reg(t)), k, false, 0xF))
		return XSC_NONE;
	const int64_t sk = xsdk_xsc_emit_scalar(l, XSC_SCALAR_SQRT, xsdk_xsc_fresh(l), xsc_reg(k), 3, 0xF);
	if (sk == XSC_NONE)
		return XSC_NONE;
	const int64_t coef = xsdk_xsc_fresh(l);
	if (!xsc_emit3(l, XSC_VECTOR_MAD, eta, xsc_temp(d), xsc_temp(sk), coef, false, 0xF))
		return XSC_NONE;
	const int64_t base = xsdk_xsc_fresh(l);
	if (!xsc_emit2(l, XSC_VECTOR_MUL, eta, xsc_plain(oi), base, false, 0xF))
		return XSC_NONE;
	const int64_t res = xsdk_xsc_fresh(l);
	XscOperand zero;
	if (!xsc_emit3(l, XSC_VECTOR_MAD, xsc_negated(on), xsc_temp(coef), xsc_temp(base), res, false, 0xF) ||
		!splat(l, 0.0, &zero))
		return XSC_NONE;
	const int64_t msk = xsdk_xsc_fresh(l);
	return done(xsc_emit2(l, XSC_VECTOR_SGE, xsc_temp(k), xsc_plain(zero), msk, false, 0xF) &&
			xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(res), xsc_temp(msk), dst, export_, 0xF),
		dst);
}

static int64_t clamp(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	double lo, hi;
	if (!xsdk_xsc_const_scalar(l, arg(ins, 1), &lo) || !xsdk_xsc_const_scalar(l, arg(ins, 2), &hi))
		return XSC_NONE;
	if (lo == 0.0 && hi == 1.0)
		return xsdk_xsc_emit_clamped(l, arg(ins, 0), dst, export_);
	XscOperand ox, lo_op, hi_op;
	if (!arg_operand(l, ins, 0, &ox) || !splat(l, lo, &lo_op) || !splat(l, hi, &hi_op))
		return XSC_NONE;
	const int64_t t = xsdk_xsc_fresh(l);
	return done(xsc_emit2(l, XSC_VECTOR_MAX, xsc_plain(ox), xsc_plain(lo_op), t, false, 0xF) &&
			xsc_emit2(l, XSC_VECTOR_MIN, xsc_temp(t), xsc_plain(hi_op), dst, export_, 0xF),
		dst);
}

static int64_t arc_cosine(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	const int64_t r = asin_reg(l, arg(ins, 0));
	XscOperand half_pi;
	if (r == XSC_NONE || !splat(l, HALF_PI, &half_pi))
		return XSC_NONE;
	return done(xsc_emit2(l, XSC_VECTOR_ADD, xsc_plain(half_pi), xsc_negated(xsc_reg(r)), dst, export_, 0xF), dst);
}

static int64_t math_op(XscLower *l, const XscInstr *ins, uint32_t inst, int64_t dst, bool export_)
{
	switch (inst) {
	case GLSL_ROUND:
	case GLSL_ROUND_EVEN:
		return round_half_up(l, ins, dst, export_);
	case GLSL_RADIANS:
		return scale(l, ins, 0.0174532925, dst, export_);
	case GLSL_DEGREES:
		return scale(l, ins, 57.2957795, dst, export_);
	case GLSL_STEP:
		return step(l, ins, dst, export_);
	case GLSL_SMOOTH_STEP:
		return smooth_step(l, ins, dst, export_);
	case GLSL_CROSS:
		return cross(l, ins, dst, export_);
	case GLSL_DETERMINANT:
		return copied(l, xsdk_xsc_emit_determinant(l, arg(ins, 0)), dst, export_);
	case GLSL_REFRACT:
		return refract(l, ins, dst, export_);
	case GLSL_F_CLAMP:
		return clamp(l, ins, dst, export_);
	case GLSL_ASIN:
		return copied(l, asin_reg(l, arg(ins, 0)), dst, export_);
	case GLSL_ACOS:
		return arc_cosine(l, ins, dst, export_);
	case GLSL_ATAN:
		return copied(l, atan_reg(l, arg(ins, 0)), dst, export_);
	default:
		break;
	}
	xsdk_xsc_lower_refuse(l, "v1 %s: unhandled GLSL.std.450 #%u", xsc_stage_name(l), (unsigned)inst);
	return XSC_NONE;
}

int64_t xsdk_xsc_extinst(XscLower *l, const XscInstr *ins, int64_t dst, bool export_)
{
	const uint32_t inst = xsdk_xsc_operand(ins, 1);
	if (dst == XSC_NONE)
		dst = xsdk_xsc_fresh(l);
	switch (inst) {
	case GLSL_F_MIX:
		return mix(l, ins, dst, export_);
	case GLSL_F_MAX:
		return binary(l, ins, XSC_VECTOR_MAX, dst, export_);
	case GLSL_F_MIN:
		return binary(l, ins, XSC_VECTOR_MIN, dst, export_);
	case GLSL_FLOOR:
		return unary(l, ins, XSC_VECTOR_FLOOR, false, dst, export_);
	case GLSL_NORMALIZE:
		return normalize(l, ins, dst, export_);
	case GLSL_LENGTH:
		return length(l, ins, dst, export_);
	case GLSL_DISTANCE:
		return distance(l, ins, dst, export_);
	case GLSL_SQRT:
		return scalar_op(l, ins, XSC_SCALAR_SQRT, dst, export_);
	case GLSL_INVERSE_SQRT:
		return scalar_op(l, ins, XSC_SCALAR_RSQ, dst, export_);
	case GLSL_EXP2:
		return scalar_op(l, ins, XSC_SCALAR_EXP, dst, export_);
	case GLSL_LOG2:
		return scalar_op(l, ins, XSC_SCALAR_LOG, dst, export_);
	case GLSL_EXP:
		return natural_exp(l, ins, dst, export_);
	case GLSL_LOG:
		return natural_log(l, ins, dst, export_);
	case GLSL_SIN:
		return sin_cos(l, ins, XSC_SCALAR_SIN, dst, export_);
	case GLSL_COS:
		return sin_cos(l, ins, XSC_SCALAR_COS, dst, export_);
	case GLSL_REFLECT:
		return reflect(l, ins, dst, export_);
	case GLSL_POW:
		return power(l, ins, dst, export_);
	case GLSL_F_ABS:
		return unary(l, ins, XSC_VECTOR_MAX, true, dst, export_);
	case GLSL_F_SIGN:
		return sign(l, ins, dst, export_);
	case GLSL_TAN:
		return tangent(l, ins, dst, export_);
	case GLSL_FRACT:
		return unary(l, ins, XSC_VECTOR_FRC, false, dst, export_);
	case GLSL_TRUNC:
		return unary(l, ins, XSC_VECTOR_TRUNC, false, dst, export_);
	case GLSL_CEIL:
		return ceiling(l, ins, dst, export_);
	default:
		return math_op(l, ins, inst, dst, export_);
	}
}
