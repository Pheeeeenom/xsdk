"""Breaks xsdk_test's checks of vulkan4xenon milestone 25's render targets in a copy of xsdk:
xsdk_test_breaks.py <copy> <set>.

Set a, one break a check of test_targets.c's formats: 2_10_10_10 resolved with red and blue swapped; the 7e3 target
drawn as 2_10_10_10, which clamps at 1; 16_16_16_16's clear words swapped, so its clear's red and green land in blue
and alpha; 16_16_FLOAT's and 16_16_16_16_FLOAT's halves resolved swapped 8 in 32, so neighbouring channels trade
places; and 32_FLOAT's floats resolved unswapped. Set b blends nothing, so 2_10_10_10's blend check fails, and
test_raster's blends with it.

Four targets at once: set c leaves RB_BLENDCONTROL1 to 3 unblended, so target 1's check fails, and counts a 64-bit
target's tiles as a 32-bit one's, so targets 2 and 3 overlap and both fail; set d gives every target target 0's write
mask, so targets 1 to 3 fail; set e resolves target 0 whichever is asked for, so all four fail.
"""
import pathlib
import sys

SETS = {
    "a": [
        ("src/gpu_resolve.c", "COPY_DEST_2_10_10_10 = 7 << COPY_DEST_FORMAT_SHIFT | COPY_DEST_ENDIAN_8IN32,",
         "COPY_DEST_2_10_10_10 = 7 << COPY_DEST_FORMAT_SHIFT | COPY_DEST_ENDIAN_8IN32 | COPY_DEST_SWAP,"),
        ("src/gpu_draw.c", "return tile | color->format << COLOR_FORMAT_SHIFT;",
         "return tile | (color->format == XSDK_TARGET_2_10_10_10_FLOAT ? XSDK_TARGET_2_10_10_10 : color->format) "
         "<< COLOR_FORMAT_SHIFT;"),
        ("src/gpu_resolve.c", "\t\twords[0] = fixed_16(rgba[0]) | fixed_16(rgba[1]) << 16;\n"
         "\t\twords[1] = fixed_16(rgba[2]) | fixed_16(rgba[3]) << 16;\n",
         "\t\twords[1] = fixed_16(rgba[0]) | fixed_16(rgba[1]) << 16;\n"
         "\t\twords[0] = fixed_16(rgba[2]) | fixed_16(rgba[3]) << 16;\n"),
        ("src/gpu_resolve.c", "COPY_DEST_16_16_FLOAT = 31 << COPY_DEST_FORMAT_SHIFT | COPY_DEST_NUMBER_FLOAT | "
         "COPY_DEST_ENDIAN_8IN16,",
         "COPY_DEST_16_16_FLOAT = 31 << COPY_DEST_FORMAT_SHIFT | COPY_DEST_NUMBER_FLOAT | COPY_DEST_ENDIAN_8IN32,"),
        ("src/gpu_resolve.c", "COPY_DEST_16_16_16_16_FLOAT = 32 << COPY_DEST_FORMAT_SHIFT | COPY_DEST_NUMBER_FLOAT | "
         "COPY_DEST_ENDIAN_8IN16,",
         "COPY_DEST_16_16_16_16_FLOAT = 32 << COPY_DEST_FORMAT_SHIFT | COPY_DEST_NUMBER_FLOAT | "
         "COPY_DEST_ENDIAN_8IN32,"),
        ("src/gpu_resolve.c", "COPY_DEST_32_FLOAT = 36 << COPY_DEST_FORMAT_SHIFT | COPY_DEST_NUMBER_FLOAT | "
         "COPY_DEST_ENDIAN_8IN32,",
         "COPY_DEST_32_FLOAT = 36 << COPY_DEST_FORMAT_SHIFT | COPY_DEST_NUMBER_FLOAT,"),
    ],
    "b": [
        ("src/gpu_draw.c", "gpu_put_register(&packets, RB_BLENDCONTROL0, state->blend_control);",
         "gpu_put_register(&packets, RB_BLENDCONTROL0, BLEND_ONE_ZERO);"),
    ],
    "c": [
        ("src/gpu_draw.c", "controls[i - 1] = states[i].blend_control;", "controls[i - 1] = BLEND_ONE_ZERO;"),
        ("src/gpu_draw.c", "return xsdk_gpu_target_tiles(width, height) << is_64_bit(format);",
         "return xsdk_gpu_target_tiles(width, height) << (is_64_bit(format) && 0);"),
    ],
    "d": [
        ("src/gpu_draw.c", "color_mask |= states[i].color_mask << COLOR_MASK_BITS * i;",
         "color_mask |= states[0].color_mask << COLOR_MASK_BITS * i;"),
    ],
    "e": [
        ("src/gpu_draw.c", "put_surface(packets, width, height, target, 1);",
         "put_surface(packets, width, height, 0, 1);"),
    ],
}


def main():
    root = pathlib.Path(sys.argv[1])
    texts = {}
    for name, old, new in SETS[sys.argv[2]]:
        path = root / name
        text = texts.get(path, path.read_text())
        assert text.count(old) == 1, (name, old)
        texts[path] = text.replace(old, new)
    for path, text in texts.items():
        path.write_text(text)
    print(f"set {sys.argv[2]}: {len(SETS[sys.argv[2]])} breaks in {len(texts)} files")


if __name__ == "__main__":
    main()
