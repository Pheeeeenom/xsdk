#include <string.h>

#include "xsc_private.h"

// The optimiser, as xenos-toolchain/xasm/optimize.py rewrites a program's instructions before allocation: common
// subexpressions, copies propagated, multiply-adds fused, values computed into their copies' places, dead code removed,
// and scalar ops co-issued.

enum {
	ALL = 0xF,
	OUTSIDE = -1,
	// At most this many different constants in an instruction (optimize.py MAX_CONSTANTS).
	MAX_CONSTANTS = 2,
	MAX_READ_KEYS = XSC_MAX_KEYS,
	MAX_DEF_KEYS = 8,
};

typedef struct {
	bool temp;
	int64_t reg;
	bool absolute;
	int components[4];
	bool negate;
} Source;

// An instruction's reads and writes as (register, component) keys, register * 4 + component.
typedef struct {
	int64_t keys[MAX_READ_KEYS];
	size_t count;
} KeySet;

typedef struct {
	Xsc *x;
	XscProgram *p;
	XscOp **ops;
	size_t count;
	size_t ops_capacity;
	int64_t key_count;
	// The flow: each key's writers, ascending, and each read's (writer, reader) pairs, by key.
	KeySet *reads;
	KeySet *defs;
	size_t sets_capacity;
	size_t *write_start;
	size_t *write_list;
	size_t *reader_start;
	int64_t *reader_writer;
	size_t *reader_list;
	size_t *fill;
	size_t key_capacity;
	size_t write_capacity;
	size_t reader_capacity;
	uint8_t *live;
} Optimizer;

// ---- the tables (optimize.py, from xenia:src/xenia/gpu/ucode.cc:16-117) ----

static int vector_reads_table(int64_t op, int lanes[3])
{
	switch (op) {
	case XSC_VECTOR_ADD:
	case XSC_VECTOR_MUL:
	case XSC_VECTOR_MAX:
	case XSC_VECTOR_MIN:
	case XSC_VECTOR_SEQ:
	case XSC_VECTOR_SGT:
	case XSC_VECTOR_SGE:
	case XSC_VECTOR_SNE:
	case XSC_VECTOR_DP4:
	case XSC_VECTOR_CUBE:
		lanes[0] = lanes[1] = ALL;
		return 2;
	case XSC_VECTOR_FRC:
	case XSC_VECTOR_TRUNC:
	case XSC_VECTOR_FLOOR:
	case XSC_VECTOR_MAX4:
		lanes[0] = ALL;
		return 1;
	case XSC_VECTOR_MAD:
	case XSC_VECTOR_CND_EQ:
	case XSC_VECTOR_CND_GE:
	case XSC_VECTOR_CND_GT:
		lanes[0] = lanes[1] = lanes[2] = ALL;
		return 3;
	case XSC_VECTOR_DP3:
		lanes[0] = lanes[1] = 0x7;
		return 2;
	case XSC_VECTOR_DP2_ADD:
		lanes[0] = lanes[1] = 0x3;
		lanes[2] = 0x1;
		return 3;
	case XSC_VECTOR_DST:
		lanes[0] = 0x6;
		lanes[1] = 0xA;
		return 2;
	default:
		return -1;
	}
}

static bool elementwise(int64_t op)
{
	return (op >= XSC_VECTOR_ADD && op <= XSC_VECTOR_MAD) || (op >= XSC_VECTOR_CND_EQ && op <= XSC_VECTOR_CND_GT);
}

static bool vector_state(int64_t op)
{
	return (op >= XSC_VECTOR_SETP_EQ_PUSH && op <= XSC_VECTOR_KILL_NE) || op == XSC_VECTOR_MAX_A;
}

// Scalar ops with one operand: 2 for a and b, 1 for a alone, 0 for any other op.
static int scalar_one(int64_t op)
{
	if (op == XSC_SCALAR_ADDS || op == XSC_SCALAR_MULS || op == XSC_SCALAR_MAXS || op == XSC_SCALAR_MINS ||
		op == XSC_SCALAR_SUBS)
		return 2;
	if ((op >= XSC_SCALAR_SEQS && op <= XSC_SCALAR_RSQ) || op == XSC_SCALAR_SQRT || op == XSC_SCALAR_SIN ||
		op == XSC_SCALAR_COS)
		return 1;
	return 0;
}

static bool scalar_previous(int64_t op)
{
	return op == XSC_SCALAR_ADDS_PREV || op == XSC_SCALAR_MULS_PREV || op == XSC_SCALAR_MULS_PREV2 ||
		op == XSC_SCALAR_SUBS_PREV;
}

// ---- an instruction's fields ----

static Source source(const XscOp *op, int n)
{
	const XscAlu *const f = &op->alu;
	Source s = { .temp = f->src_is_temp[n - 1], .reg = f->src_reg[n - 1], .negate = f->src_negate[n - 1] };
	s.absolute = s.temp && (s.reg & XSC_ABS_SOURCE);
	if (s.temp)
		s.reg &= XSC_ABS_SOURCE - 1;
	for (int lane = 0; lane < 4; lane++)
		s.components[lane] = (int)((f->src_swizzle[n - 1] >> (2 * lane)) + lane) & 3;
	return s;
}

static void set_source(XscOp *op, int n, const Source *s)
{
	XscAlu *const f = &op->alu;
	f->src_reg[n - 1] = s->reg | (s->absolute ? XSC_ABS_SOURCE : 0);
	f->src_is_temp[n - 1] = s->temp;
	int64_t swizzle = 0;
	for (int lane = 0; lane < 4; lane++)
		swizzle += (int64_t)((s->components[lane] - lane) & 3) << (2 * lane);
	f->src_swizzle[n - 1] = swizzle;
	f->src_negate[n - 1] = s->negate;
}

static bool same_value(const Source *a, const Source *b)
{
	return a->temp == b->temp && a->reg == b->reg && a->absolute == b->absolute && a->negate == b->negate &&
		!memcmp(a->components, b->components, sizeof a->components);
}

static bool is_alu(const XscOp *op)
{
	return op->kind == XSC_OP_ALU;
}

static int64_t vector_mask(const XscOp *op)
{
	int64_t mask = op->alu.vector_write_mask;
	if (op->alu.is_export)
		mask &= ~op->alu.scalar_write_mask;
	return mask;
}

static bool vector_active(const XscOp *op)
{
	return vector_mask(op) != 0 || vector_state(op->alu.vector_opcode);
}

static bool scalar_active(const XscOp *op)
{
	return op->alu.scalar_opcode != XSC_SCALAR_RETAIN_PREV;
}

// The lanes the vector pipe reads of operands 1 to 3, 0 for none.
static void vector_reads(const XscOp *op, int64_t lanes[4])
{
	lanes[1] = lanes[2] = lanes[3] = 0;
	if (!vector_active(op))
		return;
	int table[3];
	const int n = vector_reads_table(op->alu.vector_opcode, table);
	if (n < 0) {
		lanes[1] = lanes[2] = lanes[3] = ALL;
		return;
	}
	for (int i = 0; i < n; i++)
		lanes[i + 1] = elementwise(op->alu.vector_opcode) ? vector_mask(op) : table[i];
}

// The lanes both pipes read of each operand; an operand read for no lane by a pipe that names it still counts.
static void reads(const XscOp *op, int64_t lanes[4], bool named[4])
{
	memset(named, 0, 4 * sizeof *named);
	lanes[1] = lanes[2] = lanes[3] = 0;
	if (!is_alu(op))
		return;
	if (vector_active(op)) {
		int table[3];
		const int n = vector_reads_table(op->alu.vector_opcode, table);
		const int count = n < 0 ? 3 : n;
		vector_reads(op, lanes);
		for (int i = 1; i <= count; i++)
			named[i] = true;
	}
	if (scalar_active(op)) {
		const int one = scalar_one(op->alu.scalar_opcode);
		lanes[3] |= one == 0 ? ALL : one == 2 ? 0x9 : 0x8;
		named[3] = true;
	}
}

static void add_key(KeySet *set, int64_t key)
{
	for (size_t i = 0; i < set->count; i++) {
		if (set->keys[i] == key)
			return;
	}
	set->keys[set->count++] = key;
}

static bool has_key(const KeySet *set, int64_t key)
{
	for (size_t i = 0; i < set->count; i++) {
		if (set->keys[i] == key)
			return true;
	}
	return false;
}

static bool intersects(const KeySet *a, const KeySet *b)
{
	for (size_t i = 0; i < a->count; i++) {
		if (has_key(b, a->keys[i]))
			return true;
	}
	return false;
}

static int64_t key(int64_t reg, int component)
{
	return reg * 4 + component;
}

// The keys an op reads; a fetch reads its source whole (optimize.py temp_reads).
static void temp_reads(const XscOp *op, KeySet *out)
{
	out->count = 0;
	if (!is_alu(op)) {
		for (int c = 0; c < 4; c++)
			add_key(out, key(op->fetch.src, c));
		return;
	}
	int64_t lanes[4];
	bool named[4];
	reads(op, lanes, named);
	for (int n = 1; n <= 3; n++) {
		const Source s = source(op, n);
		if (!named[n] || !s.temp)
			continue;
		for (int lane = 0; lane < 4; lane++) {
			if (lanes[n] >> lane & 1)
				add_key(out, key(s.reg, s.components[lane]));
		}
	}
}

// The keys an op writes; a fetch's kept components (swizzle 7) aren't (optimize.py defs).
static void defs(const XscOp *op, KeySet *out)
{
	out->count = 0;
	if (!is_alu(op)) {
		for (int c = 0; c < 4; c++) {
			if ((op->fetch.dest_swizzle >> (3 * c) & 7) != 7)
				add_key(out, key(op->fetch.dest, c));
		}
		return;
	}
	const XscAlu *const f = &op->alu;
	if (f->is_export)
		return;
	for (int c = 0; c < 4; c++) {
		if (f->vector_write_mask >> c & 1)
			add_key(out, key(f->vector_dest, c));
	}
	for (int c = 0; c < 4; c++) {
		if (f->scalar_write_mask >> c & 1)
			add_key(out, key(f->scalar_dest, c));
	}
}

static bool constants_fit(const XscOp *op)
{
	int64_t lanes[4];
	bool named[4];
	reads(op, lanes, named);
	int64_t consts[4];
	bool has[4] = { false, false, false, false };
	int64_t distinct[3];
	int distinct_count = 0;
	int count = 0;
	for (int n = 1; n <= 3; n++) {
		const Source s = source(op, n);
		if (!named[n] || s.temp)
			continue;
		consts[n] = s.reg;
		has[n] = true;
		count++;
		bool seen = false;
		for (int i = 0; i < distinct_count; i++)
			seen = seen || distinct[i] == s.reg;
		if (!seen)
			distinct[distinct_count++] = s.reg;
	}
	if (distinct_count > MAX_CONSTANTS)
		return false;
	return count < 3 || (has[2] && has[3] && consts[2] == consts[3]);
}

static bool fixed(const XscOp *op)
{
	if (!is_alu(op))
		return true;
	return vector_state(op->alu.vector_opcode) || (scalar_active(op) && !scalar_one(op->alu.scalar_opcode));
}

static bool is_move(const XscOp *op)
{
	if (fixed(op) || op->alu.is_export || scalar_active(op))
		return false;
	if (op->alu.vector_opcode != XSC_VECTOR_MAX || op->alu.vector_clamp || !op->alu.vector_write_mask)
		return false;
	const Source a = source(op, 1);
	const Source b = source(op, 2);
	return same_value(&a, &b);
}

static bool plain_vector(const XscOp *op)
{
	return !fixed(op) && !op->alu.is_export && !scalar_active(op) && op->alu.vector_write_mask != 0;
}

// ---- the program ----

static bool flatten(Optimizer *o)
{
	o->count = 0;
	for (size_t si = 0; si < o->p->stage_count; si++) {
		for (size_t i = 0; i < o->p->stages[si].count; i++) {
			if (!xsdk_xsc_room(o->x, &o->ops, o->count, &o->ops_capacity, sizeof *o->ops))
				return false;
			o->ops[o->count++] = o->p->stages[si].ops[i];
		}
	}
	return true;
}

static void remove_ops(Optimizer *o, XscOp *const *doomed, size_t doomed_count)
{
	for (size_t si = 0; si < o->p->stage_count; si++) {
		XscStage *const stage = &o->p->stages[si];
		size_t kept = 0;
		for (size_t i = 0; i < stage->count; i++) {
			bool gone = false;
			for (size_t d = 0; d < doomed_count; d++)
				gone = gone || stage->ops[i] == doomed[d];
			if (!gone)
				stage->ops[kept++] = stage->ops[i];
		}
		stage->count = kept;
	}
}

static bool grow(Xsc *x, void *items, size_t *capacity, size_t needed, size_t item_bytes)
{
	while (*capacity < needed) {
		if (!xsdk_xsc_room(x, items, *capacity, capacity, item_bytes))
			return false;
	}
	return true;
}

// Which instruction's write each key holds at each instruction, and each write's readers (optimize.py _Flow).
static bool build_flow(Optimizer *o)
{
	if (!flatten(o))
		return false;
	const size_t n = o->count;
	const size_t keys = (size_t)o->key_count;
	size_t sets = o->sets_capacity;
	if (!grow(o->x, &o->reads, &sets, n + 1, sizeof *o->reads))
		return false;
	sets = o->sets_capacity;
	if (!grow(o->x, &o->defs, &sets, n + 1, sizeof *o->defs))
		return false;
	o->sets_capacity = sets;
	size_t key_capacity = o->key_capacity;
	bool ok = grow(o->x, &o->write_start, &key_capacity, keys + 1, sizeof *o->write_start);
	key_capacity = o->key_capacity;
	ok = ok && grow(o->x, &o->reader_start, &key_capacity, keys + 1, sizeof *o->reader_start);
	key_capacity = o->key_capacity;
	ok = ok && grow(o->x, &o->fill, &key_capacity, keys + 1, sizeof *o->fill);
	if (!ok)
		return false;
	o->key_capacity = key_capacity;
	memset(o->write_start, 0, (keys + 1) * sizeof *o->write_start);
	memset(o->reader_start, 0, (keys + 1) * sizeof *o->reader_start);
	size_t total_defs = 0, total_reads = 0;
	for (size_t i = 0; i < n; i++) {
		temp_reads(o->ops[i], &o->reads[i]);
		defs(o->ops[i], &o->defs[i]);
		for (size_t k = 0; k < o->defs[i].count; k++)
			o->write_start[o->defs[i].keys[k] + 1]++;
		for (size_t k = 0; k < o->reads[i].count; k++)
			o->reader_start[o->reads[i].keys[k] + 1]++;
		total_defs += o->defs[i].count;
		total_reads += o->reads[i].count;
	}
	size_t capacity = o->write_capacity;
	ok = grow(o->x, &o->write_list, &capacity, total_defs + 1, sizeof *o->write_list);
	o->write_capacity = capacity;
	capacity = o->reader_capacity;
	ok = ok && grow(o->x, &o->reader_list, &capacity, total_reads + 1, sizeof *o->reader_list);
	capacity = o->reader_capacity;
	ok = ok && grow(o->x, &o->reader_writer, &capacity, total_reads + 1, sizeof *o->reader_writer);
	if (!ok)
		return false;
	o->reader_capacity = capacity;
	for (size_t k = 0; k < keys; k++) {
		o->write_start[k + 1] += o->write_start[k];
		o->reader_start[k + 1] += o->reader_start[k];
	}
	memcpy(o->fill, o->write_start, keys * sizeof *o->fill);
	for (size_t i = 0; i < n; i++) {
		for (size_t k = 0; k < o->defs[i].count; k++)
			o->write_list[o->fill[o->defs[i].keys[k]]++] = i;
	}
	return true;
}

// The write of key that op i reads (OUTSIDE if none).
static int64_t flow_at(const Optimizer *o, size_t i, int64_t k)
{
	if (k < 0 || k >= o->key_count)
		return OUTSIDE;
	int64_t found = OUTSIDE;
	for (size_t w = o->write_start[k]; w < o->write_start[k + 1]; w++) {
		if (o->write_list[w] < i)
			found = (int64_t)o->write_list[w];
	}
	return found;
}

static void fill_readers(Optimizer *o)
{
	memcpy(o->fill, o->reader_start, (size_t)o->key_count * sizeof *o->fill);
	for (size_t i = 0; i < o->count; i++) {
		for (size_t k = 0; k < o->reads[i].count; k++) {
			const int64_t rk = o->reads[i].keys[k];
			const size_t at = o->fill[rk]++;
			o->reader_writer[at] = flow_at(o, i, rk);
			o->reader_list[at] = i;
		}
	}
}

// The readers of writer w's key, in the order they were found.
static size_t readers_of(const Optimizer *o, int64_t w, int64_t k, size_t *out, size_t out_count)
{
	if (k < 0 || k >= o->key_count)
		return out_count;
	for (size_t at = o->reader_start[k]; at < o->reader_start[k + 1]; at++) {
		if (o->reader_writer[at] == w)
			out[out_count++] = o->reader_list[at];
	}
	return out_count;
}

static bool same_values(const Optimizer *o, const Source *s, const int *components, int count, size_t i, size_t j)
{
	if (!s->temp)
		return true;
	for (int c = 0; c < count; c++) {
		if (flow_at(o, i, key(s->reg, components[c])) != flow_at(o, j, key(s->reg, components[c])))
			return false;
	}
	return true;
}

// The components a source reads for the lanes given, deduplicated as a set.
static int component_set(const Source *s, int64_t lanes_mask, int *out)
{
	int count = 0;
	for (int lane = 0; lane < 4; lane++) {
		if (!(lanes_mask >> lane & 1))
			continue;
		bool seen = false;
		for (int i = 0; i < count; i++)
			seen = seen || out[i] == s->components[lane];
		if (!seen)
			out[count++] = s->components[lane];
	}
	return count;
}

// ---- the passes; each returns whether it changed the program ----

static bool remove_dead(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!flatten(o))
		return false;
	memset(o->live, 0, (size_t)o->key_count);
	XscOp **const doomed = xsdk_xsc_alloc(o->x, (o->count + 1) * sizeof *doomed);
	if (!doomed)
		return false;
	size_t doomed_count = 0;
	for (size_t idx = o->count; idx-- > 0;) {
		XscOp *const op = o->ops[idx];
		if (is_alu(op) && !op->alu.is_export && !fixed(op)) {
			XscAlu *const f = &op->alu;
			int64_t *const masks[2] = { &f->vector_write_mask, &f->scalar_write_mask };
			const int64_t dests[2] = { f->vector_dest, f->scalar_dest };
			for (int pipe = 0; pipe < 2; pipe++) {
				int64_t keep = 0;
				for (int c = 0; c < 4; c++) {
					const int64_t k = key(dests[pipe], c);
					if ((*masks[pipe] >> c & 1) && k < o->key_count && o->live[k])
						keep |= (int64_t)1 << c;
				}
				if (keep != *masks[pipe]) {
					*masks[pipe] = keep;
					*changed = true;
				}
			}
			if (!f->scalar_write_mask && scalar_active(op)) {
				f->scalar_opcode = XSC_SCALAR_RETAIN_PREV;
				*changed = true;
			}
			if (!vector_active(op) && !scalar_active(op)) {
				doomed[doomed_count++] = op;
				continue;
			}
		}
		KeySet set;
		defs(op, &set);
		for (size_t k = 0; k < set.count; k++)
			o->live[set.keys[k]] = 0;
		temp_reads(op, &set);
		for (size_t k = 0; k < set.count; k++)
			o->live[set.keys[k]] = 1;
	}
	remove_ops(o, doomed, doomed_count);
	*changed = *changed || doomed_count;
	return true;
}

static bool same_computation(const Optimizer *o, const XscOp *a, const XscOp *b, size_t i, size_t j)
{
	const int64_t op = b->alu.vector_opcode;
	if (a->alu.vector_opcode != op || a->alu.vector_clamp != b->alu.vector_clamp)
		return false;
	int64_t lanes[4];
	vector_reads(b, lanes);
	int table[3];
	const int n = vector_reads_table(op, table);
	const int count = !vector_active(b) ? 0 : n < 0 ? 3 : n;
	for (int k = 1; k <= count; k++) {
		const Source sa = source(a, k);
		const Source sb = source(b, k);
		if (sa.temp != sb.temp || sa.reg != sb.reg || sa.absolute != sb.absolute || sa.negate != sb.negate)
			return false;
		const int64_t used = elementwise(op) ? lanes[k] : ALL;
		for (int lane = 0; lane < 4; lane++) {
			if ((used >> lane & 1) && sa.components[lane] != sb.components[lane])
				return false;
		}
		int components[4];
		const int distinct = component_set(&sa, used, components);
		if (!same_values(o, &sa, components, distinct, i, j))
			return false;
	}
	return true;
}

static bool common_subexpressions(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!build_flow(o))
		return false;
	for (size_t j = 0; j < o->count; j++) {
		XscOp *const b = o->ops[j];
		if (!plain_vector(b) || is_move(b))
			continue;
		for (size_t i = j; i-- > 0;) {
			const XscOp *const a = o->ops[i];
			if (!plain_vector(a) || !same_computation(o, a, b, i, j))
				continue;
			if (b->alu.vector_write_mask & ~a->alu.vector_write_mask)
				continue;
			// a's result must still be in its register when b runs.
			bool kept = true;
			for (int c = 0; c < 4; c++) {
				if ((b->alu.vector_write_mask >> c & 1) && flow_at(o, j, key(a->alu.vector_dest, c)) != (int64_t)i)
					kept = false;
			}
			if (!kept)
				continue;
			*changed = true;
			if (a->alu.vector_dest == b->alu.vector_dest) {
				XscOp *doomed = b;
				remove_ops(o, &doomed, 1);
				return true;
			}
			const Source move = { true, a->alu.vector_dest, false, { 0, 1, 2, 3 }, false };
			b->alu.vector_opcode = XSC_VECTOR_MAX;
			b->alu.vector_clamp = false;
			set_source(b, 1, &move);
			set_source(b, 2, &move);
			return true;
		}
	}
	return true;
}

static bool rewrite_reader(Optimizer *o, size_t i, const XscOp *mov, size_t r)
{
	XscOp *const reader = o->ops[r];
	if (fixed(reader))
		return false;
	const Source src = source(mov, 1);
	const int64_t dest = mov->alu.vector_dest;
	const int64_t mask = mov->alu.vector_write_mask;
	int64_t lanes[4];
	bool named[4];
	reads(reader, lanes, named);
	Source fresh[4];
	bool rewritten[4] = { false, false, false, false };
	bool any = false;
	for (int n = 1; n <= 3; n++) {
		const Source old = source(reader, n);
		if (!named[n] || !old.temp || old.reg != dest)
			continue;
		// Every lane of this operand that's read must come from the move.
		for (int lane = 0; lane < 4; lane++) {
			if (!(lanes[n] >> lane & 1))
				continue;
			if (!(mask >> old.components[lane] & 1) || flow_at(o, r, key(dest, old.components[lane])) != (int64_t)i)
				return false;
		}
		Source candidate = { .temp = src.temp, .reg = src.reg };
		for (int lane = 0; lane < 4; lane++)
			candidate.components[lane] = src.components[(lanes[n] >> lane & 1) ? old.components[lane] : 0];
		if (old.absolute) {
			if (!src.temp)
				return false;
			candidate.absolute = true;
			candidate.negate = old.negate;
		} else {
			candidate.absolute = src.absolute;
			candidate.negate = old.negate != src.negate;
		}
		// The move's source must hold at the reader what it held at the move.
		int components[4];
		const int distinct = component_set(&candidate, lanes[n], components);
		if (!same_values(o, &candidate, components, distinct, i, r))
			return false;
		fresh[n] = candidate;
		rewritten[n] = true;
		any = true;
	}
	if (!any)
		return false;
	const XscAlu saved = reader->alu;
	for (int n = 1; n <= 3; n++) {
		if (rewritten[n])
			set_source(reader, n, &fresh[n]);
	}
	if (!constants_fit(reader)) {
		reader->alu = saved;
		return false;
	}
	return true;
}

static void sort_unique(size_t *items, size_t *count)
{
	for (size_t i = 1; i < *count; i++) {
		const size_t moving = items[i];
		size_t j = i;
		for (; j > 0 && items[j - 1] > moving; j--)
			items[j] = items[j - 1];
		items[j] = moving;
	}
	size_t kept = 0;
	for (size_t i = 0; i < *count; i++) {
		if (!kept || items[kept - 1] != items[i])
			items[kept++] = items[i];
	}
	*count = kept;
}

static size_t users_of(Optimizer *o, size_t i, int64_t dest, int64_t mask, size_t *users)
{
	size_t count = 0;
	for (int c = 0; c < 4; c++) {
		if (mask >> c & 1)
			count = readers_of(o, (int64_t)i, key(dest, c), users, count);
	}
	sort_unique(users, &count);
	return count;
}

static bool propagate_copies(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!build_flow(o))
		return false;
	fill_readers(o);
	size_t *const users = xsdk_xsc_alloc(o->x, (4 * o->count + 1) * sizeof *users);
	if (!users)
		return false;
	for (size_t i = 0; i < o->count; i++) {
		const XscOp *const mov = o->ops[i];
		if (!is_move(mov))
			continue;
		const size_t count = users_of(o, i, mov->alu.vector_dest, mov->alu.vector_write_mask, users);
		for (size_t u = 0; u < count; u++) {
			if (rewrite_reader(o, i, mov, users[u])) {
				*changed = true;
				return true;
			}
		}
	}
	return true;
}

// The mul's sources for the add's lanes, through the product's components (optimize.py fuse_multiply_adds).
static void fused_sources(const XscOp *mul, const Source *tsrc, int64_t lanes, Source fused[2])
{
	for (int n = 1; n <= 2; n++) {
		const Source s = source(mul, n);
		fused[n - 1] = s;
		for (int lane = 0; lane < 4; lane++)
			fused[n - 1].components[lane] = s.components[(lanes >> lane & 1) ? tsrc->components[lane] : 0];
		fused[n - 1].negate = s.negate != (n == 1 && tsrc->negate);
	}
}

// Whether the add reads the product, from that mul, in exactly one operand; which through hit.
static bool one_hit(const Optimizer *o, const XscOp *add, size_t i, size_t r, int64_t t, int64_t lanes, int *hit)
{
	int hits = 0;
	for (int n = 1; n <= 2; n++) {
		const Source s = source(add, n);
		if (!s.temp || s.reg != t)
			continue;
		bool from_mul = false;
		for (int lane = 0; lane < 4; lane++) {
			if ((lanes >> lane & 1) && flow_at(o, r, key(t, s.components[lane])) == (int64_t)i)
				from_mul = true;
		}
		if (from_mul) {
			*hit = n;
			hits++;
		}
	}
	return hits == 1;
}

static bool try_fuse(Optimizer *o, size_t i, size_t *users)
{
	const XscOp *const mul = o->ops[i];
	if (!plain_vector(mul) || mul->alu.vector_opcode != XSC_VECTOR_MUL || mul->alu.vector_clamp)
		return false;
	const int64_t t = mul->alu.vector_dest;
	const int64_t tmask = mul->alu.vector_write_mask;
	if (users_of(o, i, t, tmask, users) != 1)
		return false;
	const size_t r = users[0];
	XscOp *const add = o->ops[r];
	if (fixed(add) || scalar_active(add) || add->alu.vector_opcode != XSC_VECTOR_ADD)
		return false;
	const int64_t lanes = vector_mask(add);
	int hit;
	if (!one_hit(o, add, i, r, t, lanes, &hit))
		return false;
	const Source tsrc = source(add, hit);
	const Source other = source(add, 3 - hit);
	if (tsrc.absolute)
		return false;
	for (int lane = 0; lane < 4; lane++) {
		if ((lanes >> lane & 1) &&
			(!(tmask >> tsrc.components[lane] & 1) || flow_at(o, r, key(t, tsrc.components[lane])) != (int64_t)i))
			return false;
	}
	Source fused[2];
	fused_sources(mul, &tsrc, lanes, fused);
	// a and b must hold at the add what they held at the multiply.
	for (int n = 0; n < 2; n++) {
		int components[4];
		const int distinct = component_set(&fused[n], lanes, components);
		if (!same_values(o, &fused[n], components, distinct, i, r))
			return false;
	}
	const XscAlu saved = add->alu;
	add->alu.vector_opcode = XSC_VECTOR_MAD;
	set_source(add, 1, &fused[0]);
	set_source(add, 2, &fused[1]);
	set_source(add, 3, &other);
	if (!constants_fit(add)) {
		add->alu = saved;
		return false;
	}
	return true;
}

static bool fuse_multiply_adds(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!build_flow(o))
		return false;
	fill_readers(o);
	size_t *const users = xsdk_xsc_alloc(o->x, (4 * o->count + 1) * sizeof *users);
	if (!users)
		return false;
	for (size_t i = 0; i < o->count; i++) {
		if (try_fuse(o, i, users)) {
			*changed = true;
			return true;
		}
	}
	return true;
}

static bool vector_only(const XscOp *op)
{
	if (!plain_vector(op))
		return false;
	int64_t lanes[4];
	vector_reads(op, lanes);
	int table[3];
	const int n = vector_reads_table(op->alu.vector_opcode, table);
	// Operand 3 is in the vector reads when the op names three, or isn't in the table.
	return !(vector_active(op) && (n < 0 || n == 3));
}

static bool scalar_only(const XscOp *op)
{
	return !fixed(op) && !op->alu.is_export && scalar_active(op) && !vector_active(op) &&
		op->alu.scalar_write_mask != 0;
}

// Puts x's working pipe into y, which keeps its own (optimize.py merge).
static bool merge(XscOp *y, const XscOp *x)
{
	const bool x_vector = vector_only(x);
	const XscOp *const vec = x_vector ? x : y;
	const XscOp *const sca = x_vector ? y : x;
	XscAlu merged = vec->alu;
	merged.scalar_opcode = sca->alu.scalar_opcode;
	merged.scalar_dest = sca->alu.scalar_dest;
	merged.scalar_write_mask = sca->alu.scalar_write_mask;
	merged.scalar_clamp = sca->alu.scalar_clamp;
	merged.is_scalar_dest_relative = sca->alu.is_scalar_dest_relative;
	merged.src_reg[2] = sca->alu.src_reg[2];
	merged.src_is_temp[2] = sca->alu.src_is_temp[2];
	merged.src_swizzle[2] = sca->alu.src_swizzle[2];
	merged.src_negate[2] = sca->alu.src_negate[2];
	const XscAlu saved = y->alu;
	y->alu = merged;
	if (!constants_fit(y)) {
		y->alu = saved;
		return false;
	}
	return true;
}

static size_t stage_of(const XscProgram *p, const XscOp *op)
{
	for (size_t si = 0; si < p->stage_count; si++) {
		for (size_t i = 0; i < p->stages[si].count; i++) {
			if (p->stages[si].ops[i] == op)
				return si;
		}
	}
	return SIZE_MAX;
}

// Whether x joined an earlier op y, searching back from j (optimize.py co_issue's inner loop).
static bool join_earlier(Optimizer *o, size_t j)
{
	XscOp *const x = o->ops[j];
	const bool vector = vector_only(x);
	KeySet x_reads, x_defs;
	temp_reads(x, &x_reads);
	defs(x, &x_defs);
	const size_t x_stage = stage_of(o->p, x);
	for (size_t i = j; i-- > 0;) {
		XscOp *const y = o->ops[i];
		if (stage_of(o->p, y) != x_stage)
			return false;
		KeySet y_defs, y_reads;
		defs(y, &y_defs);
		// Joined, both pipes read before either writes: x mustn't read what y writes.
		if (intersects(&x_reads, &y_defs))
			return false;
		const bool partner = vector ? scalar_only(y) : vector_only(y);
		if (partner && !intersects(&x_defs, &y_defs) && merge(y, x)) {
			XscOp *doomed = x;
			remove_ops(o, &doomed, 1);
			return true;
		}
		// Moving x above y: y mustn't read or write what x writes, nor change state.
		temp_reads(y, &y_reads);
		if (intersects(&x_defs, &y_reads) || intersects(&x_defs, &y_defs) || (is_alu(y) && fixed(y)))
			return false;
	}
	return false;
}

static bool co_issue(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!flatten(o))
		return false;
	for (size_t j = 0; j < o->count; j++) {
		const XscOp *const x = o->ops[j];
		if (!vector_only(x) && !scalar_only(x))
			continue;
		if (join_earlier(o, j)) {
			*changed = true;
			return true;
		}
	}
	return true;
}

// Ops whose result is the same in every lane (xenia:src/xenia/gpu/shader_interpreter.cc:436-519, 614-618).
static bool replicates(int64_t op)
{
	return op == XSC_VECTOR_DP4 || op == XSC_VECTOR_DP3 || op == XSC_VECTOR_DP2_ADD || op == XSC_VECTOR_MAX4;
}

// A max of one temp with itself into a temp or an export, clamped or not, alone in its instruction.
static bool is_copy(const XscOp *op)
{
	if (fixed(op) || scalar_active(op) || op->alu.vector_opcode != XSC_VECTOR_MAX || !op->alu.vector_write_mask)
		return false;
	if (op->alu.is_export && (op->alu.scalar_write_mask || op->alu.is_scalar_dest_relative))
		return false;
	const Source a = source(op, 1);
	const Source b = source(op, 2);
	return same_value(&a, &b) && a.temp && !a.absolute && !a.negate;
}

// Whether nothing in the program writes key k, though it writes k's register: the value there is undefined.
static bool undefined(const Optimizer *o, int64_t k)
{
	const int64_t first = k - k % 4;
	if (k < 0 || first + 4 > o->key_count)
		return false;
	return o->write_start[k] == o->write_start[k + 1] && o->write_start[first] != o->write_start[first + 4];
}

// A writer of what the copy at j copies, and the copy's lanes it gives, which the copy reads through s.
typedef struct {
	size_t i;
	size_t j;
	Source s;
	int64_t lanes;
} Place;

static int64_t result_reg(const XscOp *w)
{
	return vector_active(w) ? w->alu.vector_dest : w->alu.scalar_dest;
}

static int64_t result_mask(const XscOp *w)
{
	return vector_active(w) ? w->alu.vector_write_mask : w->alu.scalar_write_mask;
}

// The copy's lane that will hold component c of the writer's result, or -1.
static int holder(const Optimizer *o, const Place *pl, int c)
{
	const XscOp *const w = o->ops[pl->i];
	// A scalar result fills each lane it writes (xenia:src/xenia/gpu/shader_interpreter.cc:948-955).
	const bool same = !vector_active(w) || replicates(w->alu.vector_opcode);
	for (int lane = 0; lane < 4; lane++) {
		if ((pl->lanes >> lane & 1) && (same || pl->s.components[lane] == c))
			return lane;
	}
	return -1;
}

// Whether a vector op's result fits the copy's lanes: swizzled to them, the same in all, or in its own lanes.
static bool lands(const Optimizer *o, const Place *pl)
{
	const int64_t op = o->ops[pl->i]->alu.vector_opcode;
	if (elementwise(op) || replicates(op))
		return true;
	for (int lane = 0; lane < 4; lane++) {
		if ((pl->lanes >> lane & 1) && pl->s.components[lane] != lane)
			return false;
	}
	return true;
}

// Whether nothing from the writer to the copy reads or writes the copy's lanes, nor the writer reads them.
static bool place_free(const Optimizer *o, const Place *pl)
{
	const XscAlu *const copy = &o->ops[pl->j]->alu;
	// An export's writer moves to the copy instead, so nothing between may write what it reads.
	if (copy->is_export) {
		for (size_t k = pl->i + 1; k < pl->j; k++) {
			if (intersects(&o->defs[k], &o->reads[pl->i]))
				return false;
		}
		return true;
	}
	KeySet place = { .count = 0 };
	for (int c = 0; c < 4; c++) {
		if (pl->lanes >> c & 1)
			add_key(&place, key(copy->vector_dest, c));
	}
	if (intersects(&o->reads[pl->i], &place))
		return false;
	for (size_t k = pl->i + 1; k < pl->j; k++) {
		if (intersects(&o->reads[k], &place) || intersects(&o->defs[k], &place))
			return false;
	}
	return true;
}

// Whether nothing between from and to but skip writes key k.
static bool unwritten(const Optimizer *o, size_t from, size_t to, size_t skip, int64_t k)
{
	for (size_t at = from + 1; at < to; at++) {
		if (at != skip && has_key(&o->defs[at], k))
			return false;
	}
	return true;
}

// Whether operand n of op r reads the writer's result: its first lane read does, as can_redirect leaves no other.
static bool reads_result(const Optimizer *o, const Place *pl, size_t r, int n, const int64_t lanes[4])
{
	const Source s = source(o->ops[r], n);
	const int64_t t = result_reg(o->ops[pl->i]);
	for (int lane = 0; s.temp && s.reg == t && lane < 4; lane++) {
		if (lanes[n] >> lane & 1)
			return flow_at(o, r, key(t, s.components[lane])) == (int64_t)pl->i;
	}
	return false;
}

// Whether op r, another reader of the writer's result, can read it from the copy's place instead.
static bool can_redirect(const Optimizer *o, const Place *pl, size_t r)
{
	const XscOp *const reader = o->ops[r];
	const int64_t t = result_reg(o->ops[pl->i]);
	const int64_t d = o->ops[pl->j]->alu.vector_dest;
	if (fixed(reader) || d == t)
		return false;
	int64_t lanes[4];
	bool named[4];
	reads(reader, lanes, named);
	for (int n = 1; n <= 3; n++) {
		const Source s = source(reader, n);
		if (!named[n] || !s.temp || s.reg != t)
			continue;
		// An operand reads the result alone or none of it.
		int ours = 0, others = 0;
		for (int lane = 0; lane < 4; lane++) {
			if (!(lanes[n] >> lane & 1))
				continue;
			if (flow_at(o, r, key(t, s.components[lane])) != (int64_t)pl->i) {
				others++;
				continue;
			}
			const int h = holder(o, pl, s.components[lane]);
			if (h < 0 || !unwritten(o, pl->i, r, pl->j, key(d, h)))
				return false;
			ours++;
		}
		if (ours && others)
			return false;
	}
	return true;
}

static void redirect(Optimizer *o, const Place *pl, size_t r)
{
	XscOp *const reader = o->ops[r];
	int64_t lanes[4];
	bool named[4];
	reads(reader, lanes, named);
	for (int n = 1; n <= 3; n++) {
		if (!named[n] || !reads_result(o, pl, r, n, lanes))
			continue;
		const Source old = source(reader, n);
		Source moved = old;
		moved.reg = o->ops[pl->j]->alu.vector_dest;
		// A lane the reader doesn't read may hold anything.
		for (int lane = 0; lane < 4; lane++) {
			const int h = holder(o, pl, old.components[lane]);
			moved.components[lane] = h < 0 ? lane : h;
		}
		set_source(reader, n, &moved);
	}
}

// Whether the writer can write the copy's lanes itself, its other readers finding its result there unclamped.
static bool can_place(Optimizer *o, const Place *pl, size_t *users)
{
	const XscOp *const w = o->ops[pl->i];
	const XscOp *const copy = o->ops[pl->j];
	const bool vector = plain_vector(w);
	if (!vector && !scalar_only(w))
		return false;
	// A vector op exports after the export's alloc, as the lowering's do (xsc_lower.c, xsdk_xsc_emit_construct).
	if (copy->alu.is_export && (!vector || stage_of(o->p, w) != stage_of(o->p, copy)))
		return false;
	if ((vector && !lands(o, pl)) || !place_free(o, pl))
		return false;
	const size_t count = users_of(o, pl->i, result_reg(w), result_mask(w), users);
	for (size_t u = 0; u < count; u++) {
		if (users[u] != pl->j && (copy->alu.is_export || copy->alu.vector_clamp || !can_redirect(o, pl, users[u])))
			return false;
	}
	return true;
}

// Moves op w down its stage to just before op copy, so the exports stay where they were.
static void sink(XscProgram *p, XscOp *w, const XscOp *copy)
{
	XscStage *const stage = &p->stages[stage_of(p, w)];
	size_t at = 0;
	while (stage->ops[at] != w)
		at++;
	for (; stage->ops[at + 1] != copy; at++)
		stage->ops[at] = stage->ops[at + 1];
	stage->ops[at] = w;
}

// The writer writes the copy's lanes, clamped as the copy is (xenia:src/xenia/gpu/shader_interpreter.cc:902-909).
static void place(Optimizer *o, const Place *pl, size_t *users)
{
	XscOp *const w = o->ops[pl->i];
	const XscAlu *const c = &o->ops[pl->j]->alu;
	const size_t count = users_of(o, pl->i, result_reg(w), result_mask(w), users);
	for (size_t u = 0; u < count; u++) {
		if (users[u] != pl->j)
			redirect(o, pl, users[u]);
	}
	XscAlu *const f = &w->alu;
	if (!vector_active(w)) {
		f->scalar_dest = c->vector_dest;
		f->scalar_write_mask = pl->lanes;
		f->scalar_clamp = f->scalar_clamp || c->vector_clamp;
		return;
	}
	int table[3];
	const int operands = vector_reads_table(f->vector_opcode, table);
	for (int n = 1; elementwise(f->vector_opcode) && n <= operands; n++) {
		const Source old = source(w, n);
		Source moved = old;
		for (int lane = 0; lane < 4; lane++)
			moved.components[lane] = old.components[pl->s.components[lane]];
		set_source(w, n, &moved);
	}
	f->vector_dest = c->vector_dest;
	f->vector_write_mask = pl->lanes;
	f->is_export = c->is_export;
	f->vector_clamp = f->vector_clamp || c->vector_clamp;
	if (c->is_export)
		sink(o->p, w, o->ops[pl->j]);
}

// The copy's lanes whose value writer w gave.
static int64_t lanes_from(const int64_t writers[4], int64_t mask, int64_t w)
{
	int64_t lanes = 0;
	for (int lane = 0; lane < 4; lane++) {
		if ((mask >> lane & 1) && writers[lane] == w)
			lanes |= (int64_t)1 << lane;
	}
	return lanes;
}

// Each lane's writer, an undefined lane taking a defined one's; false where a lane has none.
static bool copy_writers(const Optimizer *o, size_t j, Source *s, int64_t writers[4])
{
	const int64_t mask = o->ops[j]->alu.vector_write_mask;
	int defined = -1;
	for (int lane = 0; lane < 4; lane++) {
		writers[lane] = (mask >> lane & 1) ? flow_at(o, j, key(s->reg, s->components[lane])) : OUTSIDE;
		if (writers[lane] != OUTSIDE && defined < 0)
			defined = lane;
	}
	for (int lane = 0; lane < 4; lane++) {
		if (!(mask >> lane & 1) || writers[lane] != OUTSIDE)
			continue;
		if (defined < 0 || !undefined(o, key(s->reg, s->components[lane])))
			return false;
		s->components[lane] = s->components[defined];
		writers[lane] = writers[defined];
	}
	return true;
}

// Whether the copy at j went, each instruction it copies from now writing its lanes.
static bool try_place(Optimizer *o, size_t j, size_t *users)
{
	XscOp *const copy = o->ops[j];
	if (!is_copy(copy))
		return false;
	Source s = source(copy, 1);
	int64_t writers[4];
	if (!copy_writers(o, j, &s, writers))
		return false;
	const int64_t mask = copy->alu.vector_write_mask;
	Place places[4];
	int count = 0;
	int64_t seen = 0;
	for (int lane = 0; lane < 4; lane++) {
		if (!(mask >> lane & 1) || (seen >> lane & 1))
			continue;
		places[count] = (Place){ (size_t)writers[lane], j, s, lanes_from(writers, mask, writers[lane]) };
		seen |= places[count].lanes;
		if (!can_place(o, &places[count++], users))
			return false;
	}
	for (int n = 0; n < count; n++)
		place(o, &places[n], users);
	XscOp *doomed = copy;
	remove_ops(o, &doomed, 1);
	return true;
}

static bool compute_in_place(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!build_flow(o))
		return false;
	fill_readers(o);
	size_t *const users = xsdk_xsc_alloc(o->x, (4 * o->count + 1) * sizeof *users);
	if (!users)
		return false;
	for (size_t j = 0; j < o->count; j++) {
		if (try_place(o, j, users)) {
			*changed = true;
			return true;
		}
	}
	return true;
}

static const bool *refused_flags(const XscAlu *f, bool flags[9])
{
	flags[0] = f->is_vector_dest_relative;
	flags[1] = f->is_const_0_addressed;
	flags[2] = f->is_const_1_addressed;
	flags[3] = f->is_const_address_register_relative;
	flags[4] = f->src_const_rel[0];
	flags[5] = f->src_const_rel[1];
	flags[6] = f->src_const_rel[2];
	flags[7] = f->is_predicated;
	flags[8] = f->abs_constants;
	return flags;
}

// Programs the passes leave alone (optimize.py can_optimize).
static bool can_optimize(const XscProgram *p)
{
	if (!p->whole_sources || p->loop_count)
		return false;
	for (size_t si = 0; si < p->stage_count; si++) {
		for (size_t i = 0; i < p->stages[si].count; i++) {
			const XscOp *const op = p->stages[si].ops[i];
			if (!is_alu(op)) {
				if (op->fetch.is_src_relative || op->fetch.is_dest_relative || op->fetch.is_predicated)
					return false;
				continue;
			}
			const XscAlu *const f = &op->alu;
			bool flags[9];
			refused_flags(f, flags);
			for (int k = 0; k < 9; k++) {
				if (flags[k])
					return false;
			}
			int64_t scalar_mask = f->scalar_write_mask;
			if (f->is_export)
				scalar_mask &= ~f->vector_write_mask;
			if (scalar_previous(f->scalar_opcode) || (f->scalar_opcode == XSC_SCALAR_RETAIN_PREV && scalar_mask))
				return false;
			if (f->is_scalar_dest_relative && !f->is_export)
				return false;
		}
	}
	return true;
}

static bool first_round(Optimizer *o, bool *changed)
{
	if (!remove_dead(o, changed) || *changed)
		return o->x->status == XSDK_XSC_OK;
	if (!common_subexpressions(o, changed) || *changed)
		return o->x->status == XSDK_XSC_OK;
	if (!propagate_copies(o, changed) || *changed)
		return o->x->status == XSDK_XSC_OK;
	if (!fuse_multiply_adds(o, changed) || *changed)
		return o->x->status == XSDK_XSC_OK;
	return compute_in_place(o, changed);
}

bool xsdk_xsc_optimize(Xsc *x, XscProgram *p, bool *ran)
{
	*ran = can_optimize(p);
	if (!*ran)
		return true;
	Optimizer o = { .x = x, .p = p, .key_count = (p->max_reg + 2) * 4 };
	o.live = xsdk_xsc_alloc(x, (size_t)o.key_count + 1);
	if (!o.live)
		return false;
	bool changed = true;
	while (changed) {
		if (!first_round(&o, &changed))
			return false;
	}
	changed = true;
	while (changed) {
		if (!co_issue(&o, &changed))
			return false;
	}
	return remove_dead(&o, &changed);
}

// ---- what the allocator needs to pack values into components (xsc_program.c) ----

size_t xsdk_xsc_read_keys(const XscOp *op, int64_t keys[XSC_MAX_KEYS])
{
	KeySet set;
	temp_reads(op, &set);
	memcpy(keys, set.keys, set.count * sizeof *keys);
	return set.count;
}

size_t xsdk_xsc_write_keys(const XscOp *op, int64_t keys[XSC_MAX_KEYS])
{
	KeySet set;
	defs(op, &set);
	memcpy(keys, set.keys, set.count * sizeof *keys);
	return set.count;
}

bool xsdk_xsc_lanes_fixed(const XscOp *op)
{
	if (!is_alu(op) || op->alu.is_export || !op->alu.vector_write_mask || replicates(op->alu.vector_opcode))
		return false;
	// An elementwise op's operands move with its lanes, but a scalar op beside it reads operand 3 in its own.
	int table[3];
	return !elementwise(op->alu.vector_opcode) ||
		(scalar_active(op) && vector_reads_table(op->alu.vector_opcode, table) == 3);
}

static int64_t moved_mask(int64_t mask, const uint8_t lanes[4])
{
	int64_t moved = 0;
	for (int c = 0; c < 4; c++) {
		if (mask >> c & 1)
			moved |= (int64_t)1 << lanes[c];
	}
	return moved;
}

void xsdk_xsc_move_lanes(XscOp *op, const uint8_t (*lanes)[4], int64_t count)
{
	if (!is_alu(op))
		return;
	XscAlu *const f = &op->alu;
	static const uint8_t same[4] = { 0, 1, 2, 3 };
	const bool vector_temp = !f->is_export && f->vector_write_mask && f->vector_dest < count;
	const uint8_t *const to = vector_temp ? lanes[f->vector_dest] : same;
	int table[3];
	const int operands = elementwise(f->vector_opcode) ? vector_reads_table(f->vector_opcode, table) : 0;
	for (int n = 1; n <= 3; n++) {
		const Source s = source(op, n);
		Source moved = s;
		// An elementwise result's lane takes its operands' same lane.
		for (int lane = 0; n <= operands && lane < 4; lane++)
			moved.components[to[lane]] = s.components[lane];
		for (int lane = 0; moved.temp && moved.reg < count && lane < 4; lane++)
			moved.components[lane] = lanes[moved.reg][moved.components[lane]];
		set_source(op, n, &moved);
	}
	if (vector_temp)
		f->vector_write_mask = moved_mask(f->vector_write_mask, to);
	if (!f->is_export && f->scalar_write_mask && f->scalar_dest < count)
		f->scalar_write_mask = moved_mask(f->scalar_write_mask, lanes[f->scalar_dest]);
}
