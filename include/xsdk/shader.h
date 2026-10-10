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
	// Vertex data formats (xenia:src/xenia/gpu/xenos.h:602-617).
	XSDK_VERTEX_FLOAT2 = 37,
	XSDK_VERTEX_FLOAT4 = 38,
};

typedef struct {
	volatile uint32_t *microcode;
	uint32_t address;
	uint32_t dword_count;
	uint32_t type;
	// SQ_PROGRAM_CNTL and SQ_CONTEXT_MISC bits the shader needs.
	uint32_t program_control;
	uint32_t context_misc;
	// The container's vertex fetch patch records; the container must outlive the shader.
	const uint32_t *fetches;
	uint32_t fetch_count;
} XsdkShader;

// One attribute of a vertex: the shader input it feeds, its data format, and where it sits in the vertex.
typedef struct {
	uint32_t usage;
	uint32_t usage_index;
	uint32_t format;
	uint32_t offset_bytes;
} XsdkVertexElement;

// Copies the container's microcode into GPU memory.
NTSTATUS xsdk_shader_load(XsdkShader *shader, const uint32_t *container, uint32_t size_bytes);
// Patches a vertex shader's fetches for this layout, as libxenon does; not while the GPU may be running it.
NTSTATUS xsdk_shader_set_vertex_layout(
	XsdkShader *shader, const XsdkVertexElement *elements, uint32_t element_count, uint32_t stride_bytes);
void xsdk_shader_free(XsdkShader *shader);

#ifdef __cplusplus
}
#endif

#endif
