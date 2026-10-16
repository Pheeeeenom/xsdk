#ifndef XSDK_SHADER_H
#define XSDK_SHADER_H

#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shaders from xost's containers, libxenon's format (xenos-toolchain/docs/container.md), placed in GPU memory.

enum {
	XSDK_SHADER_VERTEX = 0,
	XSDK_SHADER_PIXEL = 1,
	// Declaration usages, numbered as D3D does (libxenon:drivers/xenos/xe.h:159-169).
	XSDK_USAGE_POSITION = 0,
	XSDK_USAGE_TEXCOORD = 5,
	XSDK_USAGE_COLOR = 10,
	// Each takes a vertex fetch constant, 95 down.
	XSDK_VERTEX_STREAMS = 16,
};

// Vertex formats, as Vulkan's; vertices are 32-bit words, each component after the first in the bits above it.
enum {
	XSDK_VERTEX_FLOAT1,
	XSDK_VERTEX_FLOAT2,
	XSDK_VERTEX_FLOAT3,
	XSDK_VERTEX_FLOAT4,
	// Half floats, two to a word.
	XSDK_VERTEX_HALF2,
	XSDK_VERTEX_HALF4,
	// Four bytes to a word: unsigned or signed, normalised to 0 to 1 or -1 to 1, or whole numbers.
	XSDK_VERTEX_UBYTE4N,
	XSDK_VERTEX_BYTE4N,
	XSDK_VERTEX_UBYTE4,
	XSDK_VERTEX_BYTE4,
	// Two 16-bit numbers to a word, likewise.
	XSDK_VERTEX_USHORT2N,
	XSDK_VERTEX_SHORT2N,
	XSDK_VERTEX_USHORT2,
	XSDK_VERTEX_SHORT2,
	XSDK_VERTEX_USHORT4N,
	XSDK_VERTEX_SHORT4N,
	XSDK_VERTEX_USHORT4,
	XSDK_VERTEX_SHORT4,
	XSDK_VERTEX_FORMATS,
};

typedef struct {
	volatile uint32_t *microcode;
	uint32_t address;
	uint32_t dword_count;
	uint32_t type;
	// SQ_PROGRAM_CNTL and SQ_CONTEXT_MISC bits the shader needs.
	uint32_t program_control;
	uint32_t context_misc;
	// The container's vertex fetch patch records and microcode; the container must outlive the shader.
	const uint32_t *fetches;
	uint32_t fetch_count;
	const uint32_t *source;
} XsdkShader;

// Which of a format's components an attribute's x, y, z or w reads, counted as the formats above lay them out; 0 reads
// its own, so x reads the first.
enum {
	XSDK_COMPONENT_OWN,
	XSDK_COMPONENT_X,
	XSDK_COMPONENT_Y,
	XSDK_COMPONENT_Z,
	XSDK_COMPONENT_W,
};

// One attribute of a vertex: the shader input it feeds, its format, its stream, where it sits in the stream, and the
// XSDK_COMPONENT_* each of x to w reads, so data whose components lie in another order is fetched as it is.
typedef struct {
	uint32_t usage;
	uint32_t usage_index;
	uint32_t format;
	uint32_t stream;
	uint32_t offset_bytes;
	uint8_t components[4];
} XsdkVertexElement;

// Copies the container's microcode into GPU memory.
NTSTATUS xsdk_shader_load(XsdkShader *shader, const uint32_t *container, uint32_t size_bytes);
// Patches a vertex shader's fetches for this layout and each stream's stride, as libxenon does; not while it may run.
NTSTATUS xsdk_shader_set_vertex_layout(
	XsdkShader *shader, const XsdkVertexElement *elements, uint32_t element_count, const uint32_t *stream_strides);
// Moves the shader's texture fetches up by first_slot fetch constants, from the container's, so both stages' textures
// fit among the 32 slots; not while it may run.
NTSTATUS xsdk_shader_move_textures(XsdkShader *shader, uint32_t first_slot);
void xsdk_shader_free(XsdkShader *shader);

#ifdef __cplusplus
}
#endif

#endif
