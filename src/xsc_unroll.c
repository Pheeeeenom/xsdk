#include <math.h>
#include <stdio.h>
#include <string.h>

#include "xsc_private.h"

// Loops unrolled and locals turned into SSA values, as xenos-toolchain/xsc/unroll.py does it.

enum {
	MAX_TRIP = 4096,
	OP_I_ADD = 128,
	OP_I_SUB = 130,
	OP_I_MUL = 132,
	OP_S_DIV = 135,
	OP_I_EQUAL = 170,
	OP_S_LESS_THAN_EQUAL = 179,
};

typedef struct {
	uint32_t id;
	int64_t value;
} IntConst;

typedef struct {
	uint32_t id;
	double value;
} FloatConst;

// A block: the instructions after its label, and its terminator (NULL if the function ends first).
typedef struct {
	uint32_t label;
	size_t first;
	size_t count;
	const XscInstr *term;
} Block;

typedef struct {
	Xsc *x;
	XscModule *m;
	Block *blocks;
	size_t block_count;
	bool *func_var;
	IntConst *iconst;
	size_t iconst_count;
	size_t iconst_capacity;
	FloatConst *fconst;
	size_t fconst_count;
	size_t fconst_capacity;
	uint32_t int_tid;
	uint32_t float_tid;
	uint32_t counter;
	XscInstr *minted;
	size_t minted_count;
	size_t minted_capacity;
	XscInstr *emitted;
	size_t emitted_count;
	size_t emitted_capacity;
	// Function variable -> the value it holds (env); 0 for none.
	uint32_t *env;
	uint32_t env_limit;
	bool refused;
} Unroller;

static bool refuse(Unroller *u, const char *message)
{
	u->refused = true;
	return xsdk_xsc_fail(u->x, XSDK_XSC_REFUSED, "%s", message);
}

static bool is_func_var(const Unroller *u, uint32_t id)
{
	return id < u->m->id_limit && u->func_var[id];
}

static bool find_int(const Unroller *u, uint32_t id, int64_t *value)
{
	for (size_t i = 0; i < u->iconst_count; i++) {
		if (u->iconst[i].id == id) {
			*value = u->iconst[i].value;
			return true;
		}
	}
	return false;
}

static bool find_float(const Unroller *u, uint32_t id, double *value)
{
	for (size_t i = 0; i < u->fconst_count; i++) {
		if (u->fconst[i].id == id) {
			*value = u->fconst[i].value;
			return true;
		}
	}
	return false;
}

static uint32_t env_get(const Unroller *u, uint32_t id)
{
	return id < u->env_limit ? u->env[id] : 0;
}

// The value an id stands for: a local map's, the environment's, or the id itself (unroll.py remap's rr).
typedef struct {
	uint32_t *from;
	uint32_t *to;
	size_t count;
	size_t capacity;
} LocalMap;

static bool map_get(const LocalMap *lm, uint32_t id, uint32_t *value)
{
	for (size_t i = lm->count; i > 0; i--) {
		if (lm->from[i - 1] == id) {
			*value = lm->to[i - 1];
			return true;
		}
	}
	return false;
}

static bool map_set(Unroller *u, LocalMap *lm, uint32_t id, uint32_t value)
{
	size_t capacity = lm->capacity;
	if (!xsdk_xsc_room(u->x, &lm->from, lm->count, &capacity, sizeof *lm->from))
		return false;
	capacity = lm->capacity;
	if (!xsdk_xsc_room(u->x, &lm->to, lm->count, &capacity, sizeof *lm->to))
		return false;
	lm->capacity = capacity;
	lm->from[lm->count] = id;
	lm->to[lm->count] = value;
	lm->count++;
	return true;
}

static uint32_t through(const Unroller *u, const LocalMap *lm, uint32_t id)
{
	uint32_t value;
	if (map_get(lm, id, &value))
		return value;
	const uint32_t held = env_get(u, id);
	return held ? held : id;
}

static uint32_t local_or_self(const LocalMap *lm, uint32_t id)
{
	uint32_t value;
	return map_get(lm, id, &value) ? value : id;
}

static uint32_t fresh(Unroller *u)
{
	return u->counter++;
}

static bool add_instr(Unroller *u, XscInstr **list, size_t *count, size_t *capacity, XscInstr ins)
{
	if (!xsdk_xsc_room(u->x, list, *count, capacity, sizeof **list))
		return false;
	(*list)[(*count)++] = ins;
	return true;
}

static uint32_t *new_words(Unroller *u, size_t count)
{
	return xsdk_xsc_alloc(u->x, (count ? count : 1) * sizeof(uint32_t));
}

static bool int_id(Unroller *u, int64_t value, uint32_t *id)
{
	for (size_t i = 0; i < u->iconst_count; i++) {
		if (u->iconst[i].value == value) {
			*id = u->iconst[i].id;
			return true;
		}
	}
	if (!u->int_tid)
		return refuse(u, "no 32-bit int type to materialise a constant");
	const uint32_t nid = fresh(u);
	uint32_t *const words = new_words(u, 3);
	if (!words)
		return false;
	words[0] = u->int_tid;
	words[1] = nid;
	words[2] = (uint32_t)value;
	if (!add_instr(u, &u->minted, &u->minted_count, &u->minted_capacity,
			(XscInstr){ SPV_CONSTANT, u->int_tid, nid, words, 3 }))
		return false;
	if (!xsdk_xsc_room(u->x, &u->iconst, u->iconst_count, &u->iconst_capacity, sizeof *u->iconst))
		return false;
	u->iconst[u->iconst_count++] = (IntConst){ nid, value };
	*id = nid;
	return true;
}

static bool float_id(Unroller *u, double value, uint32_t *id)
{
	for (size_t i = 0; i < u->fconst_count; i++) {
		if (u->fconst[i].value == value) {
			*id = u->fconst[i].id;
			return true;
		}
	}
	if (!u->float_tid)
		return refuse(u, "no 32-bit float type to materialise a constant");
	const uint32_t nid = fresh(u);
	uint32_t *const words = new_words(u, 3);
	if (!words)
		return false;
	words[0] = u->float_tid;
	words[1] = nid;
	words[2] = xsdk_xsc_bits_of_float(value);
	if (!add_instr(u, &u->minted, &u->minted_count, &u->minted_capacity,
			(XscInstr){ SPV_CONSTANT, u->float_tid, nid, words, 3 }))
		return false;
	if (!xsdk_xsc_room(u->x, &u->fconst, u->fconst_count, &u->fconst_capacity, sizeof *u->fconst))
		return false;
	u->fconst[u->fconst_count++] = (FloatConst){ nid, value };
	*id = nid;
	return true;
}

static int64_t signed_literal(uint32_t literal)
{
	return literal >= 0x80000000u ? (int64_t)literal - 0x100000000LL : literal;
}

static bool read_constants(Unroller *u)
{
	const XscModule *const m = u->m;
	bool *const int_type = xsdk_xsc_alloc(u->x, m->id_limit);
	bool *const float_type = xsdk_xsc_alloc(u->x, m->id_limit);
	if (!int_type || !float_type)
		return false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		const uint32_t width = xsdk_xsc_operand(ins, 0);
		if (ins->opcode == SPV_TYPE_INT) {
			int_type[ins->result_id] = true;
			if (width == 32 && xsdk_xsc_operand(ins, 1) == 1 && !u->int_tid)
				u->int_tid = ins->result_id;
		} else if (ins->opcode == SPV_TYPE_FLOAT) {
			float_type[ins->result_id] = true;
			if (width == 32 && !u->float_tid)
				u->float_tid = ins->result_id;
		}
	}
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode != SPV_CONSTANT || ins->result_type >= m->id_limit)
			continue;
		const uint32_t literal = xsdk_xsc_operand(ins, 0);
		if (int_type[ins->result_type]) {
			if (!xsdk_xsc_room(u->x, &u->iconst, u->iconst_count, &u->iconst_capacity, sizeof *u->iconst))
				return false;
			u->iconst[u->iconst_count++] = (IntConst){ ins->result_id, signed_literal(literal) };
		} else if (float_type[ins->result_type]) {
			if (!xsdk_xsc_room(u->x, &u->fconst, u->fconst_count, &u->fconst_capacity, sizeof *u->fconst))
				return false;
			u->fconst[u->fconst_count++] = (FloatConst){ ins->result_id, xsdk_xsc_float_bits(literal) };
		}
	}
	return true;
}

static int64_t int_fold(uint32_t op, int64_t a, int64_t b)
{
	if (op == OP_I_ADD)
		return a + b;
	if (op == OP_I_SUB)
		return a - b;
	if (op == OP_I_MUL)
		return a * b;
	// SPIR-V SDiv truncates toward zero, as Python's int(a / b) of the true quotient does.
	return b != 0 ? (int64_t)((double)a / (double)b) : 0;
}

static bool int_compare(uint32_t op, int64_t a, int64_t b)
{
	switch (op) {
	case 170:
		return a == b;
	case 171:
		return a != b;
	case 172:
	case 173:
		return a > b;
	case 174:
	case 175:
		return a >= b;
	case 176:
	case 177:
		return a < b;
	default:
		return a <= b;
	}
}

static bool is_compare(uint32_t op)
{
	return op >= OP_I_EQUAL && op <= OP_S_LESS_THAN_EQUAL;
}

static bool is_return(uint32_t op)
{
	return op == SPV_KILL || op == SPV_RETURN || op == SPV_RETURN_VALUE;
}

// A copy of a value instruction with its ids remapped (unroll.py remap, clone_generic).
static bool clone_generic(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	uint32_t *const words = new_words(u, (size_t)count + 2);
	if (!words)
		return false;
	const uint32_t nid = fresh(u);
	words[0] = ins->result_type;
	words[1] = nid;
	for (uint32_t i = 0; i < count; i++) {
		// An ExtInst's instruction number, a shuffle's components and an extract's indices are literals.
		bool literal = false;
		if (ins->opcode == SPV_EXT_INST)
			literal = i == 1;
		else if (ins->opcode == SPV_VECTOR_SHUFFLE)
			literal = i >= 2;
		else if (ins->opcode == SPV_COMPOSITE_EXTRACT)
			literal = i >= 1;
		words[2 + i] = literal ? ops[i] : through(u, lm, ops[i]);
	}
	if (!add_instr(u, &u->emitted, &u->emitted_count, &u->emitted_capacity,
			(XscInstr){ ins->opcode, ins->result_type, nid, words, count + 2 }))
		return false;
	return map_set(u, lm, ins->result_id, nid);
}

static bool env_set(Unroller *u, uint32_t var, uint32_t value)
{
	if (var >= u->env_limit)
		return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "local %%%u past the module's ids", (unsigned)var);
	u->env[var] = value;
	return true;
}

static bool interpret_load(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	const uint32_t ptr = xsdk_xsc_operand(ins, 0);
	if (is_func_var(u, ptr)) {
		if (!env_get(u, ptr)) {
			char message[64];
			(void)snprintf(message, sizeof message, "local %%%u read before write", (unsigned)ptr);
			return refuse(u, message);
		}
		return map_set(u, lm, ins->result_id, env_get(u, ptr));
	}
	uint32_t *const words = new_words(u, 3);
	if (!words)
		return false;
	const uint32_t nid = fresh(u);
	words[0] = ins->result_type;
	words[1] = nid;
	words[2] = local_or_self(lm, ptr);
	if (!add_instr(u, &u->emitted, &u->emitted_count, &u->emitted_capacity,
			(XscInstr){ SPV_LOAD, ins->result_type, nid, words, 3 }))
		return false;
	return map_set(u, lm, ins->result_id, nid);
}

static bool interpret_store(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	const uint32_t target = xsdk_xsc_operand(ins, 0);
	const uint32_t value = through(u, lm, xsdk_xsc_operand(ins, 1));
	if (is_func_var(u, target))
		return env_set(u, target, value);
	uint32_t *const words = new_words(u, 2);
	if (!words)
		return false;
	words[0] = local_or_self(lm, target);
	words[1] = value;
	return add_instr(u, &u->emitted, &u->emitted_count, &u->emitted_capacity, (XscInstr){ SPV_STORE, 0, 0, words, 2 });
}

static bool interpret_int(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	const uint32_t a = local_or_self(lm, xsdk_xsc_operand(ins, 0));
	const uint32_t b = local_or_self(lm, xsdk_xsc_operand(ins, 1));
	int64_t va, vb;
	if (!find_int(u, a, &va) || !find_int(u, b, &vb))
		return refuse(u, "non-constant integer op in loop");
	uint32_t id;
	if (!int_id(u, int_fold(ins->opcode, va, vb), &id))
		return false;
	return map_set(u, lm, ins->result_id, id);
}

static bool interpret_float(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	const uint32_t a = local_or_self(lm, xsdk_xsc_operand(ins, 0));
	double va, vb;
	const bool a_const = find_float(u, a, &va);
	uint32_t id;
	if (ins->opcode == SPV_F_NEGATE) {
		if (!a_const)
			return clone_generic(u, ins, lm);
		if (!float_id(u, -va, &id))
			return false;
		return map_set(u, lm, ins->result_id, id);
	}
	const uint32_t b = local_or_self(lm, xsdk_xsc_operand(ins, 1));
	const bool b_const = find_float(u, b, &vb);
	if (!a_const || !b_const || (ins->opcode == SPV_F_DIV && vb == 0.0))
		return clone_generic(u, ins, lm);
	double r;
	if (ins->opcode == SPV_F_ADD)
		r = va + vb;
	else if (ins->opcode == SPV_F_SUB)
		r = va - vb;
	else if (ins->opcode == SPV_F_MUL)
		r = va * vb;
	else
		r = va / vb;
	if (!float_id(u, r, &id))
		return false;
	return map_set(u, lm, ins->result_id, id);
}

static bool interpret_convert(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	const uint32_t a = local_or_self(lm, xsdk_xsc_operand(ins, 0));
	uint32_t id;
	if (ins->opcode == SPV_CONVERT_S_TO_F) {
		int64_t v;
		if (!find_int(u, a, &v))
			return refuse(u, "ConvertSToF of non-constant in loop");
		if (!float_id(u, (double)v, &id))
			return false;
	} else {
		double v;
		if (!find_float(u, a, &v))
			return refuse(u, "ConvertFToS of non-constant in loop");
		if (!int_id(u, (int64_t)trunc(v), &id))
			return false;
	}
	return map_set(u, lm, ins->result_id, id);
}

// Symbolically executes a straight-line block (unroll.py interpret).
static bool interpret(Unroller *u, size_t first, size_t count, LocalMap *lm)
{
	for (size_t i = first; i < first + count; i++) {
		const XscInstr *const ins = &u->m->instrs[i];
		const uint32_t op = ins->opcode;
		bool ok = true;
		if (op == SPV_VARIABLE || is_compare(op))
			continue;
		if (op == SPV_LOAD)
			ok = interpret_load(u, ins, lm);
		else if (op == SPV_STORE)
			ok = interpret_store(u, ins, lm);
		else if (op == SPV_ACCESS_CHAIN && is_func_var(u, xsdk_xsc_operand(ins, 0)))
			ok = refuse(u, "indexed access into a local not supported");
		else if (op == OP_I_ADD || op == OP_I_SUB || op == OP_I_MUL || op == OP_S_DIV)
			ok = interpret_int(u, ins, lm);
		else if (op == SPV_CONVERT_S_TO_F || op == SPV_CONVERT_F_TO_S)
			ok = interpret_convert(u, ins, lm);
		else if (op == SPV_F_NEGATE || op == SPV_F_ADD || op == SPV_F_SUB || op == SPV_F_MUL || op == SPV_F_DIV)
			ok = interpret_float(u, ins, lm);
		else if (!ins->result_id)
			continue;
		else
			ok = clone_generic(u, ins, lm);
		if (!ok)
			return false;
	}
	return true;
}

static const Block *block_of(const Unroller *u, uint32_t label)
{
	for (size_t i = 0; i < u->block_count; i++) {
		if (u->blocks[i].label == label)
			return &u->blocks[i];
	}
	return NULL;
}

static bool block_has(const Unroller *u, const Block *b, uint32_t opcode, const XscInstr **found)
{
	for (size_t i = b->first; i < b->first + b->count; i++) {
		if (u->m->instrs[i].opcode == opcode) {
			if (found)
				*found = &u->m->instrs[i];
			return true;
		}
	}
	return false;
}

static bool walk(Unroller *u, uint32_t start, uint32_t stop, bool has_stop);

static bool constant_int(const Unroller *u, uint32_t id, int64_t *value)
{
	if (find_int(u, id, value))
		return true;
	const XscInstr *const lx = xsdk_xsc_by_result(u->m, id);
	if (lx && lx->opcode == SPV_LOAD) {
		const uint32_t held = env_get(u, xsdk_xsc_operand(lx, 0));
		return held && find_int(u, held, value);
	}
	return false;
}

// A comparison folded to a constant with the current environment; false through known if it's data-dependent.
static bool const_bool(const Unroller *u, uint32_t ssa, bool *known)
{
	*known = false;
	const XscInstr *const c = xsdk_xsc_by_result(u->m, ssa);
	if (!c || !is_compare(c->opcode))
		return false;
	int64_t va, vb;
	if (!constant_int(u, xsdk_xsc_operand(c, 0), &va) || !constant_int(u, xsdk_xsc_operand(c, 1), &vb))
		return false;
	*known = true;
	return int_compare(c->opcode, va, vb);
}

typedef struct {
	uint32_t iv;
	int64_t bound;
	bool iv_left;
	bool body_when_true;
	uint32_t compare;
} LoopTest;

static bool loop_runs(const Unroller *u, const LoopTest *t)
{
	int64_t val = 0;
	(void)find_int(u, env_get(u, t->iv), &val);
	const bool r = t->iv_left ? int_compare(t->compare, val, t->bound) : int_compare(t->compare, t->bound, val);
	return t->body_when_true ? r : !r;
}

static bool loop_test(Unroller *u, const XscInstr *cterm, uint32_t merge, LoopTest *t, uint32_t *body)
{
	const uint32_t cond = xsdk_xsc_operand(cterm, 0);
	const uint32_t t_lbl = xsdk_xsc_operand(cterm, 1);
	const uint32_t f_lbl = xsdk_xsc_operand(cterm, 2);
	if (merge != t_lbl && merge != f_lbl)
		return refuse(u, "loop exit does not target the merge block");
	*body = t_lbl == merge ? f_lbl : t_lbl;
	t->body_when_true = *body == t_lbl;
	const XscInstr *const cmp = xsdk_xsc_by_result(u->m, cond);
	if (!cmp || !is_compare(cmp->opcode))
		return refuse(u, "loop condition is not an integer comparison");
	t->compare = cmp->opcode;
	const uint32_t a = xsdk_xsc_operand(cmp, 0);
	const uint32_t b = xsdk_xsc_operand(cmp, 1);
	const XscInstr *const la = xsdk_xsc_by_result(u->m, a);
	const XscInstr *const lb = xsdk_xsc_by_result(u->m, b);
	if (la && la->opcode == SPV_LOAD && is_func_var(u, xsdk_xsc_operand(la, 0)) && find_int(u, b, &t->bound)) {
		t->iv = xsdk_xsc_operand(la, 0);
		t->iv_left = true;
	} else if (lb && lb->opcode == SPV_LOAD && is_func_var(u, xsdk_xsc_operand(lb, 0)) && find_int(u, a, &t->bound)) {
		t->iv = xsdk_xsc_operand(lb, 0);
		t->iv_left = false;
	} else {
		return refuse(u, "condition not (induction var vs constant)");
	}
	int64_t ignored;
	if (!env_get(u, t->iv) || !find_int(u, env_get(u, t->iv), &ignored))
		return refuse(u, "induction var not constant on loop entry");
	return true;
}

// Unrolls the loop whose header is header_lbl; its merge label through merge (unroll.py expand_loop).
static bool expand_loop(Unroller *u, const Block *header, uint32_t *merge)
{
	const XscInstr *lm_ins = NULL;
	(void)block_has(u, header, SPV_LOOP_MERGE, &lm_ins);
	*merge = xsdk_xsc_operand(lm_ins, 0);
	const uint32_t continue_lbl = xsdk_xsc_operand(lm_ins, 1);
	if (!header->term || header->term->opcode != SPV_BRANCH)
		return refuse(u, "unexpected loop header terminator");
	const Block *const cond_block = block_of(u, xsdk_xsc_operand(header->term, 0));
	if (!cond_block)
		return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "no loop condition block");
	if (!cond_block->term || cond_block->term->opcode != SPV_BRANCH_CONDITIONAL)
		return refuse(u, "loop condition is not a BranchConditional");
	LoopTest test = { 0 };
	uint32_t body = 0;
	if (!loop_test(u, cond_block->term, *merge, &test, &body))
		return false;
	const Block *const cont = block_of(u, continue_lbl);
	if (!cont)
		return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "no continue block");
	int trips = 0;
	while (loop_runs(u, &test)) {
		if (++trips > MAX_TRIP)
			return refuse(u, "loop trip count exceeds the unroll cap");
		const uint32_t before = env_get(u, test.iv);
		LocalMap lm = { 0 };
		if (!walk(u, body, continue_lbl, true) || !interpret(u, cont->first, cont->count, &lm))
			return false;
		if (env_get(u, test.iv) == before)
			return refuse(u, "loop induction variable does not advance");
	}
	return true;
}

static bool walk(Unroller *u, uint32_t start, uint32_t stop, bool has_stop)
{
	uint32_t cur = start;
	bool live = true;
	while (live && !(has_stop && cur == stop)) {
		const Block *const b = block_of(u, cur);
		if (!b)
			return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "%u", (unsigned)cur);
		if (block_has(u, b, SPV_LOOP_MERGE, NULL)) {
			if (!expand_loop(u, b, &cur))
				return false;
			continue;
		}
		LocalMap lm = { 0 };
		if (!interpret(u, b->first, b->count, &lm))
			return false;
		if (!b->term || is_return(b->term->opcode)) {
			live = false;
		} else if (b->term->opcode == SPV_BRANCH) {
			cur = xsdk_xsc_operand(b->term, 0);
		} else {
			// A structured if/else on a constant condition: the taken arm, then the merge.
			bool known;
			const bool value = const_bool(u, xsdk_xsc_operand(b->term, 0), &known);
			const XscInstr *sel = NULL;
			if (!known || !block_has(u, b, SPV_SELECTION_MERGE, &sel))
				return refuse(u, "data-dependent if/else in a loop body (v1)");
			const uint32_t merge = xsdk_xsc_operand(sel, 0);
			const uint32_t taken = xsdk_xsc_operand(b->term, value ? 1 : 2);
			if (taken != merge && !walk(u, taken, merge, true))
				return false;
			cur = merge;
		}
	}
	return true;
}

static bool find_blocks(Unroller *u, size_t entry, size_t end)
{
	const XscModule *const m = u->m;
	size_t capacity = 0;
	size_t i = entry;
	while (i < end) {
		if (m->instrs[i].opcode != SPV_LABEL) {
			i++;
			continue;
		}
		size_t j = i + 1;
		while (j < end && m->instrs[j].opcode != SPV_BRANCH && m->instrs[j].opcode != SPV_BRANCH_CONDITIONAL &&
			!is_return(m->instrs[j].opcode))
			j++;
		if (!xsdk_xsc_room(u->x, &u->blocks, u->block_count, &capacity, sizeof *u->blocks))
			return false;
		u->blocks[u->block_count++] =
			(Block){ m->instrs[i].result_id, i + 1, j - i - 1, j < end ? &m->instrs[j] : NULL };
		i = j + 1;
	}
	return true;
}

static bool find_index(const XscModule *m, size_t from, size_t to, uint32_t opcode, size_t *index)
{
	for (size_t i = from; i < to; i++) {
		if (m->instrs[i].opcode == opcode) {
			*index = i;
			return true;
		}
	}
	return false;
}

static bool append_all(Unroller *u, XscInstr **list, size_t *count, size_t *capacity, const XscInstr *from, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (!add_instr(u, list, count, capacity, from[i]))
			return false;
	}
	return true;
}

static bool rewrite_function(Unroller *u)
{
	XscModule *const m = u->m;
	size_t func_idx, end_idx, entry_idx;
	if (!find_index(m, 0, m->instr_count, SPV_FUNCTION, &func_idx) ||
		!find_index(m, 0, m->instr_count, SPV_FUNCTION_END, &end_idx) ||
		!find_index(m, func_idx, end_idx, SPV_LABEL, &entry_idx))
		return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "a function without its parts");
	if (!find_blocks(u, entry_idx, end_idx))
		return false;
	u->func_var = xsdk_xsc_alloc(u->x, m->id_limit);
	u->env_limit = m->id_limit;
	u->env = xsdk_xsc_alloc(u->x, (size_t)m->id_limit * sizeof *u->env);
	if (!u->func_var || !u->env || !read_constants(u))
		return false;
	for (size_t i = func_idx; i < end_idx; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode == SPV_VARIABLE && xsdk_xsc_operand(ins, 0) == SPV_STORAGE_FUNCTION)
			u->func_var[ins->result_id] = true;
	}
	u->counter = m->bound;
	if (!walk(u, m->instrs[entry_idx].result_id, 0, false))
		return false;
	// head | minted constants | the function's start and entry label | locals | body | the function's end
	XscInstr *list = NULL;
	size_t count = 0;
	size_t capacity = 0;
	bool ok = append_all(u, &list, &count, &capacity, m->instrs, func_idx) &&
		append_all(u, &list, &count, &capacity, u->minted, u->minted_count) &&
		append_all(u, &list, &count, &capacity, &m->instrs[func_idx], 1) &&
		append_all(u, &list, &count, &capacity, &m->instrs[entry_idx], 1);
	for (size_t i = func_idx; ok && i < end_idx; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode == SPV_VARIABLE && xsdk_xsc_operand(ins, 0) == SPV_STORAGE_FUNCTION)
			ok = append_all(u, &list, &count, &capacity, ins, 1);
	}
	ok = ok && append_all(u, &list, &count, &capacity, u->emitted, u->emitted_count) &&
		append_all(u, &list, &count, &capacity, &m->instrs[end_idx], 1);
	if (!ok)
		return false;
	m->instrs = list;
	m->instr_count = count;
	m->bound = u->counter;
	return xsdk_xsc_spirv_index(u->x, m);
}

// One function, no branches, and a local stored more than once or into through an access chain.
static bool stores_locals_again(Xsc *x, const XscModule *m, bool *again)
{
	*again = false;
	size_t functions = 0;
	for (size_t i = 0; i < m->instr_count; i++) {
		functions += m->instrs[i].opcode == SPV_FUNCTION;
		if (m->instrs[i].opcode == SPV_BRANCH_CONDITIONAL)
			return true;
	}
	if (functions != 1)
		return true;
	bool *const local = xsdk_xsc_alloc(x, m->id_limit);
	bool *const stored = xsdk_xsc_alloc(x, m->id_limit);
	if (!local || !stored)
		return false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode == SPV_VARIABLE && xsdk_xsc_operand(ins, 0) == SPV_STORAGE_FUNCTION)
			local[ins->result_id] = true;
	}
	for (size_t i = 0; i < m->instr_count; i++) {
		if (m->instrs[i].opcode != SPV_STORE)
			continue;
		const uint32_t target = xsdk_xsc_operand(&m->instrs[i], 0);
		const XscInstr *const chain = xsdk_xsc_by_result(m, target);
		if (chain && chain->opcode == SPV_ACCESS_CHAIN && xsdk_xsc_operand(chain, 0) < m->id_limit &&
			local[xsdk_xsc_operand(chain, 0)])
			*again = true;
		if (target < m->id_limit && local[target]) {
			if (stored[target])
				*again = true;
			stored[target] = true;
		}
	}
	return true;
}

bool xsdk_xsc_unroll(Xsc *x, XscModule *m, bool *refused)
{
	*refused = false;
	bool loop = false;
	for (size_t i = 0; i < m->instr_count; i++)
		loop = loop || m->instrs[i].opcode == SPV_LOOP_MERGE;
	bool again = false;
	if (!loop && !stores_locals_again(x, m, &again))
		return false;
	if (!loop && !again)
		return true;
	Unroller u = { .x = x, .m = m };
	const bool ok = rewrite_function(&u);
	*refused = u.refused;
	return ok;
}
