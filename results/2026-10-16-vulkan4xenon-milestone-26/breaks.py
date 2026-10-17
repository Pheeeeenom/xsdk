"""vulkan4xenon milestone 26's breaks of xsdk_test, each a small source change that should fail the checks named
with it.

breaks.py list prints the sets; breaks.py checks <set> the checks it should fail; breaks.py apply <set> <copy> applies
it to a copy of xsdk. builds.sh builds each set's copy into a .xex of its own. Every other check should pass with each
set.
"""
import pathlib
import sys

GPU_DRAW = "src/gpu_draw.c"
XSC_VERTEX = "src/xsc_lower_vertex.c"
XSC_FRAGMENT = "src/xsc_lower_fragment.c"
GPU_SETUP = "src/gpu_setup.c"

DEPTH_ON = "depth: overlaps, the stencil mask, two-sided stencil and the cleared depth all right with the depth test on"
DEPTH_OFF = "depth: with the depth test off, the depth checks fail and only they do"
DEPTH_WORDS = "depth: the overlaps' depth resolved into a D24S8 texture, each word its depth above its stencil"
CLIP = "raster: clip planes 0, 3 and 5 cut their cells at their lines, and nothing once they're off"
XSC_CLIP = "xsc: vk_clip_distance.vert compiles into the PC's container, literals and resources"
SPRITES = ("raster: point sprites from xost's shaders compiled on the console as wide as their gl_PointSize, 24 and 64 "
           "pixels, gl_PointCoord across them from the top left")
XSC_POINT_SIZE = "xsc: vk_point_sprite.vert compiles into the PC's container, literals and resources"
XSC_POINT_COORD = "xsc: vk_point_coord.frag compiles into the PC's container, literals and resources"
FILLS = "raster: a wireframe quad draws its triangles' edges and nothing inside, a point-filled one its corners alone"

SETS = {
    # Step 1: backface_enable left clear, so the front's stencil serves both faces.
    "s1a": {
        "checks": [DEPTH_ON, DEPTH_OFF, DEPTH_WORDS],
        "breaks": [
            (GPU_DRAW, "BACKFACE_ENABLE = 1 << 7,", "BACKFACE_ENABLE = 0,"),
        ],
    },
    # Step 1: RB_STENCILREFMASK_BF given the front's reference and masks.
    "s1b": {
        "checks": [DEPTH_ON, DEPTH_OFF, DEPTH_WORDS],
        "breaks": [
            (GPU_DRAW, "const uint32_t ref_masks[] = { state->stencil_ref_mask_back, state->stencil_ref_mask };",
             "const uint32_t ref_masks[] = { state->stencil_ref_mask, state->stencil_ref_mask };"),
        ],
    },
    # Step 2: the planes' ucp_ena bits dropped, so nothing is clipped; and xost naming the next plane for each distance.
    "s2a": {
        "checks": [CLIP, XSC_CLIP],
        "breaks": [
            (GPU_DRAW, "gpu_put_register(&packets, PA_CL_CLIP_CNTL, CLIP_D3D_SPACE | mask);",
             "gpu_put_register(&packets, PA_CL_CLIP_CNTL, CLIP_D3D_SPACE);"),
            (XSC_VERTEX, ".regs = (uint32_t)n };", ".regs = (uint32_t)n + 1 };"),
        ],
    },
    # Step 2: the planes written a plane's registers on, so plane n takes plane n - 1's.
    "s2b": {
        "checks": [CLIP],
        "breaks": [
            (GPU_DRAW, "gpu_put_registers(&packets, PA_CL_UCP_0_X, values,",
             "gpu_put_registers(&packets, PA_CL_UCP_0_X + 4, values,"),
        ],
    },
    # Step 3: a vertex shader's point size clamped to 1 pixel by PA_SU_POINT_MINMAX.
    "s3a": {
        "checks": [SPRITES],
        "breaks": [
            (GPU_SETUP, "POINT_RADIUS_ANY = 0xFFFF0000,", "POINT_RADIUS_ANY = 0x00080000,"),
        ],
    },
    # Step 3: xost's gl_PointCoord read from the parameters' |xy|, the pixel's column and row.
    "s3b": {
        "checks": [SPRITES, XSC_POINT_COORD],
        "breaks": [
            (XSC_FRAGMENT, "const int64_t zw[2] = { 2, 3 };", "const int64_t zw[2] = { 0, 1 };"),
        ],
    },
    # Step 3: xost's vs_export_mode left 0 for a shader exporting a size. The compiler check fails; the sprite check
    # fails too if the console needs the mode, which Xenia doesn't read.
    "s3c": {
        "checks": [XSC_POINT_SIZE],
        "breaks": [
            (XSC_VERTEX, "\tl->p->exports_point_size = true;\n", ""),
        ],
    },
    # Step 4: poly_mode's dual mode dropped, so every quad draws filled.
    "s4a": {
        "checks": [FILLS],
        "breaks": [
            (GPU_DRAW, "POLY_MODE_DUAL = 1 << 3,", "POLY_MODE_DUAL = 0,"),
        ],
    },
    # Step 4: the polygon types swapped, lines for points and points for lines.
    "s4b": {
        "checks": [FILLS],
        "breaks": [
            (GPU_DRAW, "fill == XSDK_FILL_WIREFRAME ? POLYGON_TYPE_LINES : POLYGON_TYPE_POINTS",
             "fill == XSDK_FILL_WIREFRAME ? POLYGON_TYPE_POINTS : POLYGON_TYPE_LINES"),
        ],
    },
}


def apply(name, copy):
    texts = {}
    for path, old, new in SETS[name]["breaks"]:
        file = copy / path
        text = texts.get(file, None)
        if text is None:
            text = file.read_text()
        if text.count(old) != 1:
            sys.exit(f"{name}: {path} holds {text.count(old)} of {old!r}")
        texts[file] = text.replace(old, new)
    for file, text in texts.items():
        file.write_text(text, newline="\n")
    print(f"{name}: {len(SETS[name]['breaks'])} breaks in {len(texts)} files")


def main():
    command = sys.argv[1]
    if command == "list":
        print(" ".join(SETS))
    elif command == "checks":
        print("\n".join(SETS[sys.argv[2]]["checks"]))
    else:
        apply(sys.argv[2], pathlib.Path(sys.argv[3]))


if __name__ == "__main__":
    main()
