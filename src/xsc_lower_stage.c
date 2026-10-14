#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xsc_lower.h"

// What lower.py's stage functions share: the store map, the locals, the literal constants and the needs.

bool xsdk_xsc_enter(XscLower *l)
{
	if (l->depth >= XSC_DEPTH_LIMIT)
		return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "maximum recursion depth exceeded");
	l->depth++;
	return true;
}

bool xsdk_xsc_store_map(Xsc *x, const XscModule *m, XscStores *stores)
{
	stores->value = xsdk_xsc_alloc(x, (size_t)m->id_limit * sizeof *stores->value);
	stores->targets = xsdk_xsc_alloc(x, (size_t)m->id_limit * sizeof *stores->targets);
	stores->target_count = 0;
	if (!stores->value || !stores->targets)
		return false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		const uint32_t target = xsdk_xsc_operand(ins, 0);
		if (ins->opcode != SPV_STORE || target >= m->id_limit)
			continue;
		if (!stores->value[target])
			stores->targets[stores->target_count++] = target;
		stores->value[target] = xsdk_xsc_operand(ins, 1);
	}
	return true;
}

bool *xsdk_xsc_local_vars(Xsc *x, const XscModule *m, size_t first, size_t end)
{
	bool *const locals = xsdk_xsc_alloc(x, m->id_limit);
	for (size_t i = first; locals && i < end; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode == SPV_VARIABLE && xsdk_xsc_operand(ins, 0) == SPV_STORAGE_FUNCTION &&
			ins->result_id < m->id_limit)
			locals[ins->result_id] = true;
	}
	return locals;
}

void xsdk_xsc_fill_local_store(XscLower *l, const XscStores *stores, const bool *locals)
{
	for (size_t i = 0; i < stores->target_count; i++) {
		const uint32_t target = stores->targets[i];
		if (locals[target])
			l->local_store[target] = stores->value[target];
	}
}

static uint32_t chain_base(const XscModule *m, uint32_t id)
{
	const XscInstr *const ins = xsdk_xsc_by_result(m, id);
	return ins && ins->opcode == SPV_ACCESS_CHAIN ? xsdk_xsc_operand(ins, 0) : 0;
}

// Refuses the locals the lowering would read wrong, as it reads each as its last store and the unroller left them.
bool xsdk_xsc_check_local_stores(
	Xsc *x, const XscModule *m, const bool *locals, uint32_t merged_local, uint32_t merge_label)
{
	bool *const stored = xsdk_xsc_alloc(x, m->id_limit);
	if (!stored)
		return false;
	bool merged = false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		const uint32_t target = xsdk_xsc_operand(ins, 0);
		if (ins->opcode == SPV_LABEL && merge_label && ins->result_id == merge_label)
			merged = true;
		if (ins->opcode == SPV_LOAD && merged_local && !merged &&
			(target == merged_local || chain_base(m, target) == merged_local))
			return xsdk_xsc_fail(
				x, XSDK_XSC_REFUSED, "local %%%u read before the if/else that assigns it ends", (unsigned)merged_local);
		if (ins->opcode != SPV_STORE)
			continue;
		const uint32_t base = chain_base(m, target);
		if (base && base < m->id_limit && locals[base])
			return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "a store into part of local %%%u", (unsigned)base);
		if (target < m->id_limit && locals[target] && target != merged_local) {
			if (stored[target])
				return xsdk_xsc_fail(x, XSDK_XSC_REFUSED,
					"local %%%u stored more than once where the unroller doesn't rewrite it", (unsigned)target);
			stored[target] = true;
		}
	}
	return true;
}

// Literals go past the constants the shader declares.
int64_t xsdk_xsc_literal_base(const XscInterface *si)
{
	int64_t base = 0;
	for (size_t i = 0; i < si->constant_count; i++) {
		const int64_t end = (int64_t)si->constants[i].reg + si->constants[i].regs;
		if (end > base)
			base = end;
	}
	return base > si->constant_end ? base : si->constant_end;
}

const char *xsdk_xsc_format(Xsc *x, const char *format, ...)
{
	char text[XSDK_XSC_MESSAGE_BYTES];
	va_list args;
	va_start(args, format);
	(void)vsnprintf(text, sizeof text, format, args);
	va_end(args);
	return xsdk_xsc_copy_text(x, text, strlen(text));
}

XscConstant xsdk_xsc_vector_constant(const char *name, int64_t reg, int64_t regs)
{
	return (XscConstant){ name, XSC_REGISTER_SET_FLOAT4, (uint32_t)reg, (uint32_t)regs, XSC_CLASS_VECTOR,
		XSC_TYPE_FLOAT_PARAMETER, 1, 4 };
}

bool xsdk_xsc_add_constant(Xsc *x, XscConstants *constants, XscConstant c)
{
	if (!xsdk_xsc_room(x, &constants->items, constants->count, &constants->capacity, sizeof *constants->items))
		return false;
	constants->items[constants->count++] = c;
	return true;
}

bool xsdk_xsc_add_need(Xsc *x, XscLowered *out, XsdkXscNeed need)
{
	if (!xsdk_xsc_room(x, &out->needs, out->need_count, &out->need_capacity, sizeof *out->needs))
		return false;
	out->needs[out->need_count++] = need;
	return true;
}

bool xsdk_xsc_literal_constants(XscLower *l, XscConstants *constants)
{
	for (size_t i = 0; i < l->pool_count; i++) {
		const int64_t reg = l->pool[i].reg;
		const char *const name = xsdk_xsc_format(l->x, "_lit%lld", (long long)reg);
		if (!name || !xsdk_xsc_add_constant(l->x, constants, xsdk_xsc_vector_constant(name, reg, 1)))
			return false;
	}
	return true;
}

bool xsdk_xsc_literal_needs(XscLower *l, XscLowered *out)
{
	for (size_t i = 0; i < l->pool_count; i++) {
		XsdkXscNeed need = { .kind = XSDK_XSC_LITERAL, .reg = (uint32_t)l->pool[i].reg, .regs = 1 };
		memcpy(need.value, l->pool[i].value, sizeof need.value);
		if (!xsdk_xsc_add_need(l->x, out, need))
			return false;
	}
	return true;
}
