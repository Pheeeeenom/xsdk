"""Breaks xsdk_test's checks of vulkan4xenon milestone 25's render targets in a copy of xsdk:
xsdk_test_breaks.py <copy> <set>.

Set a, one break a check of test_targets.c's formats: 2_10_10_10 resolved with red and blue swapped; the 7e3 target
drawn as 2_10_10_10, which clamps at 1; 16_16_16_16's clear words swapped, so its clear's red and green land in blue
and alpha; 16_16_FLOAT's and 16_16_16_16_FLOAT's halves resolved swapped 8 in 32, so neighbouring channels trade
places; and 32_FLOAT's floats resolved unswapped. Set b blends nothing, so 2_10_10_10's blend check fails, and
test_raster's blends with it.
"""
import pathlib
import sys

SETS = {
    "a": [
        ("src/gpu_resolve.c", "COPY_DEST_2_10_10_10 = 7 << COPY_DEST_FORMAT_SHIFT | COPY_DEST_ENDIAN_8IN32,",
         "COPY_DEST_2_10_10_10 = 7 << COPY_DEST_FORMAT_SHIFT | COPY_DEST_ENDIAN_8IN32 | COPY_DEST_SWAP,"),
        ("src/gpu_draw.c", "color_tile | color_target.format << COLOR_FORMAT_SHIFT,",
         "color_tile | (color_target.format == XSDK_TARGET_2_10_10_10_FLOAT ? XSDK_TARGET_2_10_10_10 : "
         "color_target.format) << COLOR_FORMAT_SHIFT,"),
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
