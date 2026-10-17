#ifndef XSDK_XSC_PRIVATE_H
#define XSDK_XSC_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xsdk/xsc.h"

// xost's compiler, ported from the Python in xenos-toolchain/xsc and xasm at xsdk 3da29f7, which each file names.

// ---- a compile's memory, freed at once, and its first error (xsc_memory.c) ----

typedef struct XscChunk XscChunk;

typedef struct {
	XscChunk *chunks;
	XsdkXscStatus status;
	char message[XSDK_XSC_MESSAGE_BYTES];
} Xsc;

// Zeroed memory that lives till xsdk_xsc_release; NULL, with the error set, if there's none.
void *xsdk_xsc_alloc(Xsc *x, size_t bytes);
// Records the first error and returns false.
bool xsdk_xsc_fail(Xsc *x, XsdkXscStatus status, const char *format, ...) __attribute__((format(printf, 3, 4)));
// Makes room for one more item in the array *items points to.
bool xsdk_xsc_room(Xsc *x, void *items, size_t count, size_t *capacity, size_t item_bytes);
char *xsdk_xsc_copy_text(Xsc *x, const char *text, size_t length_bytes);
void xsdk_xsc_release(Xsc *x);
// Python's repr of a float, and its round(value, 9).
void xsdk_xsc_float_repr(double value, char *text, size_t text_bytes);
double xsdk_xsc_round9(double value);
// A float32's value, from its bits.
double xsdk_xsc_float_bits(uint32_t bits);
uint32_t xsdk_xsc_bits_of_float(double value);

// ---- SPIR-V (xsc/spirv.py) ----

enum {
	SPV_MAGIC = 0x07230203,
	SPV_NAME = 5,
	SPV_MEMBER_NAME = 6,
	SPV_EXT_INST = 12,
	SPV_ENTRY_POINT = 15,
	SPV_TYPE_VOID = 19,
	SPV_TYPE_BOOL = 20,
	SPV_TYPE_INT = 21,
	SPV_TYPE_FLOAT = 22,
	SPV_TYPE_VECTOR = 23,
	SPV_TYPE_MATRIX = 24,
	SPV_TYPE_IMAGE = 25,
	SPV_TYPE_SAMPLER = 26,
	SPV_TYPE_SAMPLED_IMAGE = 27,
	SPV_TYPE_ARRAY = 28,
	SPV_TYPE_STRUCT = 30,
	SPV_TYPE_POINTER = 32,
	SPV_TYPE_FUNCTION = 33,
	SPV_CONSTANT_TRUE = 41,
	SPV_CONSTANT_FALSE = 42,
	SPV_CONSTANT = 43,
	SPV_CONSTANT_COMPOSITE = 44,
	SPV_FUNCTION = 54,
	SPV_FUNCTION_END = 56,
	SPV_VARIABLE = 59,
	SPV_LOAD = 61,
	SPV_STORE = 62,
	SPV_ACCESS_CHAIN = 65,
	SPV_IN_BOUNDS_ACCESS_CHAIN = 66,
	SPV_DECORATE = 71,
	SPV_MEMBER_DECORATE = 72,
	SPV_VECTOR_SHUFFLE = 79,
	SPV_COMPOSITE_CONSTRUCT = 80,
	SPV_COMPOSITE_EXTRACT = 81,
	SPV_COMPOSITE_INSERT = 82,
	SPV_TRANSPOSE = 84,
	SPV_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
	SPV_IMAGE_SAMPLE_EXPLICIT_LOD = 88,
	SPV_IMAGE_SAMPLE_DREF_IMPLICIT_LOD = 89,
	SPV_IMAGE_SAMPLE_DREF_EXPLICIT_LOD = 90,
	SPV_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD = 91,
	SPV_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD = 92,
	SPV_CONVERT_F_TO_S = 110,
	SPV_CONVERT_S_TO_F = 111,
	SPV_F_NEGATE = 127,
	SPV_I_ADD = 128,
	SPV_F_ADD = 129,
	SPV_I_SUB = 130,
	SPV_F_SUB = 131,
	SPV_I_MUL = 132,
	SPV_F_MUL = 133,
	SPV_S_DIV = 135,
	SPV_F_DIV = 136,
	SPV_F_MOD = 141,
	SPV_VECTOR_TIMES_SCALAR = 142,
	SPV_VECTOR_TIMES_MATRIX = 144,
	SPV_MATRIX_TIMES_VECTOR = 145,
	SPV_MATRIX_TIMES_MATRIX = 146,
	SPV_DOT = 148,
	SPV_F_ORD_EQUAL = 180,
	SPV_F_ORD_NOT_EQUAL = 182,
	SPV_F_ORD_LESS_THAN = 184,
	SPV_F_ORD_GREATER_THAN = 186,
	SPV_F_ORD_LESS_THAN_EQUAL = 188,
	SPV_F_ORD_GREATER_THAN_EQUAL = 190,
	SPV_DPDX = 207,
	SPV_DPDY = 208,
	SPV_FWIDTH = 209,
	SPV_SELECT = 169,
	SPV_S_LESS_THAN = 177,
	SPV_PHI = 245,
	SPV_LOOP_MERGE = 246,
	SPV_SELECTION_MERGE = 247,
	SPV_LABEL = 248,
	SPV_BRANCH = 249,
	SPV_BRANCH_CONDITIONAL = 250,
	SPV_SWITCH = 251,
	SPV_KILL = 252,
	SPV_RETURN = 253,
	SPV_RETURN_VALUE = 254,
	SPV_STORAGE_FUNCTION = 7,
};

typedef struct {
	uint32_t opcode;
	// 0 where the instruction has none.
	uint32_t result_type;
	uint32_t result_id;
	// The words after the opcode's.
	const uint32_t *words;
	uint32_t word_count;
} XscInstr;

typedef struct {
	uint32_t target;
	uint32_t member;
	uint32_t decoration;
	const uint32_t *args;
	uint32_t arg_count;
} XscDecoration;

typedef struct {
	uint32_t target;
	uint32_t member;
	const char *name;
} XscName;

typedef struct {
	uint32_t model;
	uint32_t id;
	const char *name;
	const uint32_t *interface;
	uint32_t interface_count;
} XscEntryPoint;

// An if/else on data the unroller merged: its condition, a bool, whether either arm works anything out of its own
// beyond picking values, and the ids made in its arms, first to end.
typedef struct {
	uint32_t condition;
	bool work;
	uint32_t first;
	uint32_t end;
} XscBranch;

// A select the unroller made where an if/else's arms meet, and that if/else.
typedef struct {
	uint32_t select;
	uint32_t branch;
} XscMerge;

// A local or output a hardware loop carries: its value on entry, at the top of each pass, at a pass's end, and after.
typedef struct {
	uint32_t init;
	uint32_t phi;
	uint32_t next;
	uint32_t result;
} XscCarried;

// A loop whose count a block holds, kept for a hardware loop: the access chain to that int, the induction variable at
// the top of each pass and after, from 0 by 1, what it carries, a break ending a pass where the condition is
// break_when (0 for none), and the ids made in its body, first to end.
typedef struct {
	uint32_t count;
	uint32_t counter;
	uint32_t counter_result;
	size_t carried_first;
	size_t carried_count;
	uint32_t break_condition;
	bool break_when;
	uint32_t first;
	uint32_t end;
} XscHardwareLoop;

enum {
	// What the unroller makes for a hardware loop's values, no SPIR-V opcode: a carried value at the top of a pass, and
	// after the loop. Each has a result type and id alone.
	XSC_OP_LOOP_PHI = 0x10000,
	XSC_OP_LOOP_RESULT = 0x10001,
};

typedef struct {
	uint32_t version;
	uint32_t generator;
	uint32_t bound;
	XscInstr *instrs;
	size_t instr_count;
	// by_result covers the ids below id_limit.
	uint32_t id_limit;
	int32_t *by_result;
	XscName *names;
	size_t name_count;
	XscName *member_names;
	size_t member_name_count;
	XscDecoration *decorations;
	size_t decoration_count;
	XscDecoration *member_decorations;
	size_t member_decoration_count;
	XscEntryPoint *entry_points;
	size_t entry_point_count;
	// What the unroller leaves the lowering besides its code (xsc_unroll.c).
	XscBranch *branches;
	size_t branch_count;
	XscMerge *merges;
	size_t merge_count;
	XscHardwareLoop *loops;
	size_t loop_count;
	XscCarried *carried;
	size_t carried_count;
} XscModule;

bool xsdk_xsc_spirv_parse(Xsc *x, const uint32_t *words, size_t word_count, XscModule *m);
bool xsdk_xsc_spirv_index(Xsc *x, XscModule *m);
// The instruction's operands, past its result type and id.
const uint32_t *xsdk_xsc_operands(const XscInstr *ins, uint32_t *count);
// Operand i, 0 past the last.
uint32_t xsdk_xsc_operand(const XscInstr *ins, uint32_t i);
uint32_t xsdk_xsc_operand_count(const XscInstr *ins);
const XscInstr *xsdk_xsc_by_result(const XscModule *m, uint32_t id);
// The last OpName for id, or fallback.
const char *xsdk_xsc_name(const XscModule *m, uint32_t id, const char *fallback);
const char *xsdk_xsc_member_name(const XscModule *m, uint32_t target, uint32_t member);
// The arguments of id's last decoration of a kind, or NULL.
const XscDecoration *xsdk_xsc_decoration(const XscModule *m, uint32_t id, uint32_t decoration);
const XscDecoration *xsdk_xsc_member_decoration(
	const XscModule *m, uint32_t target, uint32_t member, uint32_t decoration);
// A decoration's first argument, or fallback where there's none.
int64_t xsdk_xsc_decoration_arg(const XscDecoration *d, int64_t fallback);
// The opcode's name as Python's Instr.name gives it.
const char *xsdk_xsc_op_name(uint32_t opcode, char *buffer, size_t buffer_bytes);

// ---- types and the interface (xsc/resolve.py) ----

enum {
	SPV_DEC_ROW_MAJOR = 4,
	SPV_DEC_ARRAY_STRIDE = 6,
	SPV_DEC_MATRIX_STRIDE = 7,
	SPV_DEC_BUILTIN = 11,
	SPV_DEC_LOCATION = 30,
	SPV_DEC_BINDING = 33,
	SPV_DEC_DESCRIPTOR_SET = 34,
	SPV_DEC_OFFSET = 35,
};

typedef enum {
	XSC_TYPE_NONE,
	XSC_TYPE_VOID,
	XSC_TYPE_BOOL,
	XSC_TYPE_INT,
	XSC_TYPE_FLOAT,
	XSC_TYPE_VECTOR,
	XSC_TYPE_MATRIX,
	XSC_TYPE_IMAGE,
	XSC_TYPE_SAMPLER,
	XSC_TYPE_SAMPLED_IMAGE,
	XSC_TYPE_ARRAY,
	XSC_TYPE_STRUCT,
	XSC_TYPE_POINTER,
	XSC_TYPE_FUNCTION,
} XscTypeKind;

typedef struct {
	XscTypeKind kind;
	uint32_t comp;
	uint32_t count;
	const uint32_t *members;
	uint32_t member_count;
	uint32_t storage_class;
	// -1 where Python's is None.
	int64_t dim;
	// An image of depth, which a sample compares against (SPIR-V's Depth 1).
	bool depth;
} XscType;

enum {
	XSC_NONE = -1,
};

typedef struct {
	uint32_t vid;
	const char *name;
	uint32_t storage_class;
	// XSC_NONE where Python's is None.
	int64_t pointee;
	int64_t location;
	int64_t binding;
	int64_t builtin;
	int64_t descriptor_set;
} XscVar;

typedef enum {
	XSC_ROLE_NONE,
	XSC_ROLE_VFETCH,
	XSC_ROLE_FRAG_COORD,
	XSC_ROLE_FRONT_FACING,
	XSC_ROLE_INTERP_IN,
	XSC_ROLE_POSITION_STRUCT,
	XSC_ROLE_INTERP_OUT,
	XSC_ROLE_COLOR,
	XSC_ROLE_BLOCK,
	XSC_ROLE_SAMPLER,
	XSC_ROLE_CONSTANT,
	XSC_ROLE_ARRAY,
	// A vertex shader's gl_InstanceIndex, a float constant the runtime writes each instance's index into.
	XSC_ROLE_INSTANCE_INDEX,
	XSC_ROLE_POINT_COORD,
} XscRoleKind;

// A sampler's a and b are its slot and dim, and c the register its comparison's weights take, or XSC_NONE; a constant's
// its register and count; an array's its register, count and registers an element; a block's its index; the instance
// index's its register.
typedef struct {
	XscRoleKind kind;
	int64_t a;
	int64_t b;
	int64_t c;
} XscRole;

typedef struct {
	const XscVar *var;
	uint32_t a;
	uint32_t b;
} XscVarUse;

typedef struct {
	const char *name;
	uint32_t reg;
	uint32_t regs;
	int32_t member;
} XscConstantSlot;

typedef struct {
	bool push;
	const XscVar *var;
	uint32_t struct_type;
	uint32_t reg;
	uint32_t regs;
} XscBlock;

typedef struct {
	bool pixel;
	// vfetch: (var, usage, usage index); interp_out: (var, o, usage); interp_in: (var, r, usage); color_out: (var,
	// location); samplers: (var, slot).
	XscVarUse *vfetch;
	size_t vfetch_count;
	XscVarUse *interp_out;
	size_t interp_out_count;
	XscVarUse *interp_in;
	size_t interp_in_count;
	XscVarUse *color_out;
	size_t color_out_count;
	XscVarUse *samplers;
	size_t sampler_count;
	uint32_t position_out;
	const XscVar *frag_coord;
	const XscVar *frag_depth;
	const XscVar *front_facing;
	XscConstantSlot *constants;
	size_t constant_count;
	XscBlock *blocks;
	size_t block_count;
	uint32_t constant_end;
	// sampler_dims as (slot, dim) pairs, the last for a slot holding.
	int64_t *sampler_dims;
	size_t sampler_dim_count;
	XscRole *roles;
	uint32_t role_limit;
	// The samplers of depth images, which compare: (var, the register of their compare op's weights, slot).
	XscVarUse *comparisons;
	size_t comparison_count;
	const XscVar *point_coord;
} XscInterface;

XscType *xsdk_xsc_resolve_types(Xsc *x, const XscModule *m);
const XscType *xsdk_xsc_type(const XscModule *m, const XscType *types, uint64_t id);
const char *xsdk_xsc_type_kind_name(XscTypeKind kind);
bool xsdk_xsc_resolve_interface(Xsc *x, const XscModule *m, XscInterface *si);
const XscRole *xsdk_xsc_role(const XscInterface *si, uint32_t id);
// The register gl_InstanceIndex reads, or XSC_NONE in a shader without it.
int64_t xsdk_xsc_instance_index_reg(const XscInterface *si);
// The dim a slot's sampler has: false where Python's sampler_dims.get gives None for a missing slot.
bool xsdk_xsc_sampler_dim(const XscInterface *si, uint32_t slot, int64_t *dim);

// ---- loops unrolled (xsc/unroll.py) ----

// False with x's error set for a loop it can't unroll; refused says it was that and not memory.
bool xsdk_xsc_unroll(Xsc *x, XscModule *m, bool *refused);

// ---- instructions (xasm/encode.py, xsc/templates.py) ----

enum {
	XSC_VECTOR_ADD = 0,
	XSC_VECTOR_MUL = 1,
	XSC_VECTOR_MAX = 2,
	XSC_VECTOR_MIN = 3,
	XSC_VECTOR_SEQ = 4,
	XSC_VECTOR_SGT = 5,
	XSC_VECTOR_SGE = 6,
	XSC_VECTOR_SNE = 7,
	XSC_VECTOR_FRC = 8,
	XSC_VECTOR_TRUNC = 9,
	XSC_VECTOR_FLOOR = 10,
	XSC_VECTOR_MAD = 11,
	XSC_VECTOR_CND_EQ = 12,
	XSC_VECTOR_CND_GE = 13,
	XSC_VECTOR_CND_GT = 14,
	XSC_VECTOR_DP4 = 15,
	XSC_VECTOR_DP3 = 16,
	XSC_VECTOR_DP2_ADD = 17,
	XSC_VECTOR_CUBE = 18,
	XSC_VECTOR_MAX4 = 19,
	XSC_VECTOR_SETP_EQ_PUSH = 20,
	XSC_VECTOR_SETP_NE_PUSH = 21,
	XSC_VECTOR_SETP_GT_PUSH = 22,
	XSC_VECTOR_SETP_GE_PUSH = 23,
	XSC_VECTOR_KILL_EQ = 24,
	XSC_VECTOR_KILL_GT = 25,
	XSC_VECTOR_KILL_GE = 26,
	XSC_VECTOR_KILL_NE = 27,
	XSC_VECTOR_DST = 28,
	XSC_VECTOR_MAX_A = 29,
};

enum {
	XSC_SCALAR_ADDS = 0,
	XSC_SCALAR_ADDS_PREV = 1,
	XSC_SCALAR_MULS = 2,
	XSC_SCALAR_MULS_PREV = 3,
	XSC_SCALAR_MULS_PREV2 = 4,
	XSC_SCALAR_MAXS = 5,
	XSC_SCALAR_MINS = 6,
	XSC_SCALAR_SEQS = 7,
	XSC_SCALAR_SGTS = 8,
	XSC_SCALAR_SGES = 9,
	XSC_SCALAR_SNES = 10,
	XSC_SCALAR_FRCS = 11,
	XSC_SCALAR_TRUNCS = 12,
	XSC_SCALAR_FLOORS = 13,
	XSC_SCALAR_EXP = 14,
	XSC_SCALAR_LOGC = 15,
	XSC_SCALAR_LOG = 16,
	XSC_SCALAR_RCPC = 17,
	XSC_SCALAR_RCPF = 18,
	XSC_SCALAR_RCP = 19,
	XSC_SCALAR_RSQC = 20,
	XSC_SCALAR_RSQF = 21,
	XSC_SCALAR_RSQ = 22,
	XSC_SCALAR_MAX_AS = 23,
	XSC_SCALAR_MAX_ASF = 24,
	XSC_SCALAR_SUBS = 25,
	XSC_SCALAR_SUBS_PREV = 26,
	XSC_SCALAR_SETP_EQ = 27,
	XSC_SCALAR_SETP_NE = 28,
	XSC_SCALAR_SETP_GT = 29,
	XSC_SCALAR_SETP_GE = 30,
	XSC_SCALAR_SETP_INV = 31,
	XSC_SCALAR_SETP_POP = 32,
	XSC_SCALAR_SETP_CLR = 33,
	XSC_SCALAR_SETP_RSTR = 34,
	XSC_SCALAR_SQRT = 40,
	XSC_SCALAR_SIN = 48,
	XSC_SCALAR_COS = 49,
	XSC_SCALAR_RETAIN_PREV = 50,
};

enum {
	XSC_FETCH_VERTEX = 0,
	XSC_FETCH_TEXTURE = 1,
	XSC_FETCH_GET_BORDER_COLOR_FRAC = 16,
	XSC_FETCH_GET_COMPUTED_LOD = 17,
	XSC_FETCH_GET_GRADIENTS = 18,
	XSC_FETCH_GET_WEIGHTS = 19,
	XSC_FETCH_SET_LOD = 24,
	XSC_FETCH_SET_GRADIENTS_HORZ = 25,
	XSC_FETCH_SET_GRADIENTS_VERT = 26,
	XSC_FILTER_USE_FETCH_CONST = 3,
	XSC_DIMENSION_2D = 1,
	XSC_DIMENSION_3D = 2,
	XSC_DIMENSION_CUBE = 3,
	XSC_EXPORT_POSITION = 62,
	// x is the point's size, y and z the edge and kill flags (xenia:src/xenia/gpu/ucode.h:1813-1834).
	XSC_EXPORT_POINT_SIZE = 63,
	XSC_CF_EXEC = 1,
	XSC_CF_EXEC_END = 2,
	XSC_CF_COND_EXEC_PRED = 5,
	XSC_CF_COND_EXEC_PRED_END = 6,
	XSC_CF_LOOP_START = 7,
	XSC_CF_LOOP_END = 8,
	XSC_CF_ALLOC = 12,
};

// An ALU instruction's fields, as xasm.encode.build_alu names them.
typedef struct {
	int64_t vector_opcode;
	int64_t scalar_opcode;
	int64_t vector_dest;
	bool is_vector_dest_relative;
	int64_t vector_write_mask;
	bool vector_clamp;
	int64_t scalar_dest;
	bool is_scalar_dest_relative;
	int64_t scalar_write_mask;
	bool scalar_clamp;
	bool is_export;
	bool abs_constants;
	bool is_predicated;
	bool predicate_condition;
	bool is_const_0_addressed;
	bool is_const_1_addressed;
	bool is_const_address_register_relative;
	bool src_const_rel[3];
	int64_t src_reg[3];
	bool src_is_temp[3];
	int64_t src_swizzle[3];
	bool src_negate[3];
} XscAlu;

// A texture or vertex fetch's fields, as build_tfetch and build_vfetch_full name them.
typedef struct {
	int64_t opcode;
	int64_t src;
	int64_t src_swizzle;
	bool is_src_relative;
	int64_t dest;
	int64_t dest_swizzle;
	bool is_dest_relative;
	int64_t fetch_constant_index;
	bool fetch_valid_only;
	bool unnormalized_coordinates;
	int64_t mag_filter;
	int64_t min_filter;
	int64_t mip_filter;
	int64_t aniso_filter;
	int64_t arbitrary_filter;
	int64_t vol_mag_filter;
	int64_t vol_min_filter;
	bool use_computed_lod;
	bool use_register_lod;
	bool use_register_gradients;
	int64_t sample_location;
	double lod_bias;
	int64_t dimension;
	double offset_x;
	double offset_y;
	double offset_z;
	int64_t must_be_one;
	int64_t const_index;
	int64_t const_index_sel;
	int64_t prefetch_count;
	int64_t data_format;
	int64_t exp_adjust;
	bool is_signed;
	bool is_normalized;
	int64_t signed_rf_mode;
	bool is_index_rounded;
	int64_t stride;
	int64_t offset;
	bool is_predicated;
	bool predicate_condition;
} XscFetch;

bool xsdk_xsc_build_alu(Xsc *x, const XscAlu *f, uint32_t triple[3]);
bool xsdk_xsc_build_tfetch(Xsc *x, const XscFetch *f, uint32_t triple[3]);
bool xsdk_xsc_build_vfetch(Xsc *x, const XscFetch *f, uint32_t triple[3]);
bool xsdk_xsc_build_cf_exec(Xsc *x, uint32_t opcode, int64_t address, int64_t count, int64_t sequence, int64_t vc,
	bool is_predicate_clean, uint32_t pair[2]);
// An exec that runs where the predicate is condition (xenia:src/xenia/gpu/ucode.h:300-332).
bool xsdk_xsc_build_cf_exec_pred(Xsc *x, uint32_t opcode, int64_t address, int64_t count, int64_t sequence, int64_t vc,
	bool is_predicate_clean, bool condition, uint32_t pair[2]);
bool xsdk_xsc_build_cf_alloc(Xsc *x, int64_t alloc_type, uint32_t pair[2]);
bool xsdk_xsc_build_cf_loop_start(Xsc *x, int64_t address, int64_t loop_id, uint32_t pair[2]);
bool xsdk_xsc_build_cf_loop_end(
	Xsc *x, int64_t address, int64_t loop_id, bool is_predicated_break, bool condition, uint32_t pair[2]);
void xsdk_xsc_pack_cf_pair(const uint32_t a[2], const uint32_t b[2], uint32_t out[3]);

XscAlu xsdk_xsc_template_mov(void);
XscAlu xsdk_xsc_template_dp4(void);
XscFetch xsdk_xsc_template_vfetch(void);
XscFetch xsdk_xsc_template_tfetch(void);

// ---- containers (xdis/container_emit.py) ----

enum {
	XSC_USAGE_POSITION = 0,
	XSC_USAGE_TEXCOORD = 5,
	XSC_USAGE_COLOR = 10,
	XSC_REGISTER_SET_FLOAT4 = 2,
	XSC_REGISTER_SET_SAMPLER = 3,
	XSC_CLASS_VECTOR = 1,
	XSC_CLASS_MATRIX_COLUMNS = 3,
	XSC_CLASS_OBJECT = 4,
	XSC_TYPE_FLOAT_PARAMETER = 3,
};

typedef struct {
	const char *name;
	uint32_t register_set;
	uint32_t register_index;
	uint32_t register_count;
	uint32_t param_class;
	uint32_t param_type;
	uint32_t rows;
	uint32_t columns;
} XscConstant;

typedef struct {
	uint32_t reg;
	uint32_t usage;
	uint32_t usage_index;
	uint32_t flags;
} XscInterpolator;

typedef struct {
	uint32_t slot;
	uint32_t usage;
	uint32_t usage_index;
	uint32_t flags;
} XscVertexElement;

typedef struct {
	bool pixel;
	const uint8_t *ucode;
	size_t ucode_bytes;
	const XscConstant *constants;
	size_t constant_count;
	uint32_t program_control;
	uint32_t context_misc;
	uint32_t field10;
	const XscInterpolator *interpolators;
	size_t interpolator_count;
	const XscVertexElement *vertex_elements;
	size_t vertex_element_count;
	const int64_t *export_slots;
	size_t export_slot_count;
	uint32_t ps_field18;
	// The colours a pixel shader exports, bit n for oCn; 0 for oC0 alone.
	uint32_t pixel_outputs;
} XscContainerParts;

bool xsdk_xsc_container(Xsc *x, const XscContainerParts *parts, uint8_t **out, size_t *out_bytes);

// ---- the program builder (xasm/program.py) and optimiser (xasm/optimize.py) ----

typedef enum {
	XSC_OP_ALU,
	XSC_OP_TFETCH,
	XSC_OP_VFETCH,
} XscOpKind;

typedef struct {
	XscOpKind kind;
	bool is_fetch;
	// Whether triple holds the instruction's encoding: Python's non-empty triple.
	bool encoded;
	uint32_t triple[3];
	int64_t slot;
	// The vertex element its slot goes to, or XSC_NONE.
	int64_t vertex_element;
	XscAlu alu;
	XscFetch fetch;
} XscOp;

typedef struct {
	XscOp **ops;
	size_t count;
	size_t capacity;
	// The alloc after it, or XSC_NONE.
	int64_t alloc_after;
	// Where its ops run: everywhere, or where the predicate is true or false (XSC_PREDICATE_*).
	uint8_t predicate;
} XscStage;

typedef struct {
	size_t start;
	size_t end;
	uint32_t loop_id;
	bool predicated_break;
	bool break_condition;
} XscLoop;

enum {
	XSC_PREDICATE_NONE = 0,
	XSC_PREDICATE_TRUE = 1,
	XSC_PREDICATE_FALSE = 2,
	// A whole temp source's absolute value, past its register (program.py ABS_SOURCE).
	XSC_ABS_SOURCE = 1 << 16,
	XSC_ALLOC_POSITION = 1,
	XSC_ALLOC_INTERPOLATORS = 2,
	// A pixel shader's colors take the interpolators' alloc type.
	XSC_ALLOC_COLORS = 2,
};

typedef struct {
	bool pixel;
	uint32_t registers;
	bool whole_sources;
	bool optimize;
	bool group_fetches;
	bool clauses;
	XscStage *stages;
	size_t stage_count;
	size_t stage_capacity;
	int64_t max_reg;
	XscInterpolator *interpolators;
	size_t interpolator_count;
	size_t interpolator_capacity;
	XscVertexElement *vertex_elements;
	size_t vertex_element_count;
	size_t vertex_element_capacity;
	int64_t export_stage;
	XscLoop *loops;
	size_t loop_count;
	size_t loop_capacity;
	bool loop_open;
	size_t loop_open_stage;
	uint32_t loop_open_id;
	int64_t param_gen;
	bool exports_depth;
	// The colours a pixel shader exports, bit n for oCn.
	uint32_t color_outputs;
	bool exports_point_size;
	// The optimiser rewrote it, so the allocator packs its values into components.
	bool optimized;
	// The allocs so far: a0 doesn't survive one (docs/HARDWARE.md, "a0 doesn't survive an alloc").
	size_t alloc_count;
} XscProgram;

bool xsdk_xsc_program_init(Xsc *x, XscProgram *p, bool pixel, uint32_t registers);
XscOp *xsdk_xsc_program_alu(Xsc *x, XscProgram *p, const XscAlu *f);
XscOp *xsdk_xsc_program_tfetch(Xsc *x, XscProgram *p, const XscFetch *f);
XscOp *xsdk_xsc_program_vfetch(Xsc *x, XscProgram *p, uint32_t usage, uint32_t usage_index, const XscFetch *f);
XscOp *xsdk_xsc_program_export(Xsc *x, XscProgram *p, int64_t o, uint32_t usage, uint32_t usage_index, const XscAlu *f);
bool xsdk_xsc_program_alloc(Xsc *x, XscProgram *p, int64_t alloc_type);
bool xsdk_xsc_program_alloc_interpolators(Xsc *x, XscProgram *p);
bool xsdk_xsc_program_input(Xsc *x, XscProgram *p, int64_t r, uint32_t usage);
bool xsdk_xsc_program_pixel_parameters(Xsc *x, XscProgram *p, int64_t r);
bool xsdk_xsc_program_begin_loop(Xsc *x, XscProgram *p, uint32_t loop_id);
bool xsdk_xsc_program_end_loop(Xsc *x, XscProgram *p, bool predicated_break, bool break_condition);
// The ops after run where the predicate is true or false, or everywhere again (XSC_PREDICATE_*), a stage of their own.
bool xsdk_xsc_program_predicate(Xsc *x, XscProgram *p, uint8_t predicate);
bool xsdk_xsc_program_finish(Xsc *x, XscProgram *p, const XscConstant *constants, size_t constant_count,
	uint8_t **container, size_t *container_bytes);
// Whether an ALU op writes the predicate (program.py _modifies_predicate).
bool xsdk_xsc_modifies_predicate(const XscOp *op);
// Rewrites the program's instructions; false through ran if it leaves them alone, or only co-issues them, as it does
// where a constant is read at a run-time index (xasm/optimize.py).
bool xsdk_xsc_optimize(Xsc *x, XscProgram *p, bool *ran);

enum {
	XSC_MAX_KEYS = 12,
};

// The temps' components an op reads and writes, as register * 4 + component keys; returns the count.
size_t xsdk_xsc_read_keys(const XscOp *op, int64_t keys[XSC_MAX_KEYS]);
size_t xsdk_xsc_write_keys(const XscOp *op, int64_t keys[XSC_MAX_KEYS]);
// Whether an ALU op's vector result into a temp must stay in its lanes.
bool xsdk_xsc_lanes_fixed(const XscOp *op);
// Moves component c of each temp r below count that an ALU op names to lane lanes[r][c].
void xsdk_xsc_move_lanes(XscOp *op, const uint8_t (*lanes)[4], int64_t count);

#endif
