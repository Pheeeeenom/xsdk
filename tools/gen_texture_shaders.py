#!/usr/bin/env python3
"""Generate milestone 7's shader containers: a vertex shader that fetches a position and texture coordinates and passes
them on, and a pixel shader that samples a texture at the interpolated coordinates.

As in tools/gen_triangle_shaders.py, each field comes from Xenia's instruction definitions (xenia:src/xenia/gpu/ucode.h,
registers.h), xost's encoders pack the instructions and its emitter wraps them in libxenon's container, and nothing is
taken from a compiled shader. The layout follows xost's program builder, which its authors drew a textured quad with on
hardware: fetches in one exec, an alloc, then the exports (xenos-toolchain/xasm/program.py:17-21). Prints both
containers as C initializers, then their disassembly.
"""

import struct
import sys

import gen_triangle_shaders as tri
from gen_triangle_shaders import ce, container, enc, xu

# The loader patches the real format in (xenia:src/xenia/gpu/xenos.h:602-617).
FORMAT_32_32_FLOAT = 37
# A float2's z and w read as 0 and 1 (ucode.h:676-687).
DEST_XY01 = 0 | 1 << 3 | 4 << 6 | 5 << 9
TEXCOORD_REGISTER = 2
# The interpolated coordinates arrive in r0, and the texel goes back there.
PIXEL_REGISTER = 0
# Source components x and y, 2 bits each and absolute (xenia:src/xenia/gpu/shader_translator.cc:1132-1135).
SOURCE_XY = 0 | 1 << 2 | 2 << 4
# Texture fetch constant 0, a whole fetch group (xenos.h:1288-1315).
TEXTURE_SLOT = 0
SEQUENCE_ONE_FETCH = 0b01
USAGE_TEXCOORD = 5


def fetch(dest, usage_format, swizzle):
    return enc.build_vfetch_full(
        src=tri.INDEX_REGISTER, src_swizzle=0, dest=dest, dest_swizzle=swizzle, must_be_one=1,
        const_index=tri.VERTEX_FETCH_CONSTANT // 3, const_index_sel=tri.VERTEX_FETCH_CONSTANT % 3,
        data_format=usage_format, is_signed=False, is_normalized=False, stride=0, offset=0)


def sample(dest, source):
    # The fetch constant sets the filters; 2D, at the pixel's centre, from the base map (ucode.h:782-880).
    return enc.build_tfetch(
        src=source, src_swizzle=SOURCE_XY, dest=dest, dest_swizzle=tri.DEST_XYZW, fetch_constant_index=TEXTURE_SLOT,
        mag_filter=xu.TextureFilter.kUseFetchConst, min_filter=xu.TextureFilter.kUseFetchConst,
        mip_filter=xu.TextureFilter.kUseFetchConst, use_computed_lod=True, sample_location=xu.SampleLocation.kCenter,
        dimension=xu.FetchOpDimension.k2D)


def vertex_shader():
    # exec {fetch position, texcoord}; alloc position; exec {oPos}; alloc interpolators; exece {o0}; from slot 3.
    control_flow = [
        tri.execute(3, 2, sequence=tri.SEQUENCE_TWO_FETCHES, vc_hi=tri.VERTEX_CACHE_TWO_FETCHES),
        enc.build_cf_alloc(alloc_type=tri.ALLOC_POSITION),
        tri.execute(5, 1),
        enc.build_cf_alloc(alloc_type=tri.ALLOC_INTERPOLATORS),
        tri.execute(6, 1, end=True),
    ]
    dwords = tri.pack(control_flow, [
        fetch(tri.POSITION_REGISTER, tri.FORMAT_32_32_32_32_FLOAT, tri.DEST_XYZW),
        fetch(TEXCOORD_REGISTER, FORMAT_32_32_FLOAT, DEST_XY01),
        tri.export(tri.EXPORT_POSITION, tri.POSITION_REGISTER),
        tri.export(tri.EXPORT_INTERPOLATOR_0, TEXCOORD_REGISTER),
    ])
    return ce.build_container(
        "vertex", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(vs_num_reg=TEXCOORD_REGISTER, vs_resource=1, vs_export_count=0),
        context_misc=0,
        interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, USAGE_TEXCOORD, 0)],
        vertex_elements=[ce.VertexElement(3, tri.USAGE_POSITION, 0), ce.VertexElement(4, USAGE_TEXCOORD, 0)],
        creator=tri.CREATOR)


def pixel_shader():
    # exec {r0 = texel at r0.xy}; alloc colors; exece {oC0 = r0}; from slot 2, after two control-flow pairs.
    control_flow = [
        tri.execute(2, 1, sequence=SEQUENCE_ONE_FETCH),
        enc.build_cf_alloc(alloc_type=tri.ALLOC_COLORS),
        tri.execute(3, 1, end=True),
    ]
    dwords = tri.pack(control_flow, [sample(PIXEL_REGISTER, PIXEL_REGISTER), tri.export(tri.EXPORT_COLOR_0, 0)])
    return ce.build_container(
        "pixel", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(ps_num_reg=PIXEL_REGISTER, ps_export_mode=tri.PS_EXPORT_MODE_COLOR),
        context_misc=tri.CONTEXT_MISC_CENTROIDS_AND_CENTERS,
        interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, USAGE_TEXCOORD, 0)],
        ps_field18=tri.PS_INTERPOLATOR_WORD, creator=tri.CREATOR)


def main():
    for name, blob in (("texture_vertex_shader", vertex_shader()), ("texture_pixel_shader", pixel_shader())):
        tri.print_array(name, blob)
        parsed = container.Container(blob)
        print(f"{name}:", file=sys.stderr)
        print(xu.disassemble(parsed.ucode_dwords, parsed.shader_type), file=sys.stderr)


if __name__ == "__main__":
    main()
