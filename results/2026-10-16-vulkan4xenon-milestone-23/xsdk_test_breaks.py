"""Breaks milestone 23's xsdk_test checks in a copy of xsdk: xsdk_test_breaks.py <copy> a|b|c. builds.sh makes the
copies.

Set a, four breaks apart: a cube's base faces placed a seventh of its base apart, not a sixth (both cube groups); 3D
tiles' bank kept from flipping every four slices (the 32x32x8 group, and the w group's clamped cell at slice 7); the
border colour left out of the fetch constant (the borders group's white borders); and slices always read back from 0
(the read-back check alone).
Set b: the volume filters left nearest (the w group's cell between two slices); and every border white (the borders
group's black border, and the black border's alpha check).
Set c: w's address mode left out of the fetch constant, so w always wraps (the w group's clamped cell).
"""
import pathlib
import sys

SETS = {
    "a": [
        ("src/texture_layout.c", ".face_bytes = layout->mip_offset_bytes / layout->faces,",
         ".face_bytes = layout->mip_offset_bytes / (layout->faces + 1),"),
        ("src/tiled.c", "const uint32_t bank = (y >> 3 ^ z >> 2) & 1;", "const uint32_t bank = (y >> 3 ^ ((z >> 2) & 0)) & 1;"),
        ("src/texture.c", "sampler->border << BORDER_COLOR_SHIFT } };", "(sampler->border & 0) << BORDER_COLOR_SHIFT } };"),
        ("src/texture.c", "copy_rows(texture, level, slice, first_row, row_count, data, row_bytes, true);",
         "copy_rows(texture, level, slice * 0, first_row, row_count, data, row_bytes, true);"),
    ],
    "b": [
        ("src/texture.c", "sampler->mag_filter << VOL_MAG_FILTER_SHIFT | sampler->min_filter << VOL_MIN_FILTER_SHIFT;",
         "0u << VOL_MAG_FILTER_SHIFT | 0u << VOL_MIN_FILTER_SHIFT;"),
        ("src/texture.c", "sampler->border << BORDER_COLOR_SHIFT } };", "(sampler->border | 1) << BORDER_COLOR_SHIFT } };"),
    ],
    "c": [
        ("src/texture.c", "| sampler->address_w << CLAMP_Z_SHIFT;", "| 0u << CLAMP_Z_SHIFT;"),
    ],
}


def apply(copy, name):
    texts = {}
    for path, old, new in SETS[name]:
        if path not in texts:
            texts[path] = (copy / path).read_text()
        assert texts[path].count(old) == 1, (path, old)
        texts[path] = texts[path].replace(old, new)
    for path, text in texts.items():
        (copy / path).write_text(text)
    print(f"set {name}: {len(SETS[name])} breaks in {len(texts)} files")


apply(pathlib.Path(sys.argv[1]), sys.argv[2])
