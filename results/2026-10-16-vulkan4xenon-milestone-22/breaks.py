"""Breaks vulkan4xenon milestone 22's xsdk check in a copy of xsdk: breaks.py <copy>. builds.sh makes the copy and
builds it.

xsdk_shader_move_textures writes each texture fetch's constant back unmoved, so xsdk_test's check that the texture
test's pixel shader moves its fetch to slot 5 fails ("texture: a texture fetch moved to fetch constant 5 and back").
"""
import pathlib
import sys

BREAKS = [
    ("src/shader.c", "(word & ~TEXTURE_CONSTANT_MASK) | (constant + first_slot) << FETCH_CONSTANT_INDEX_SHIFT;",
     "(word & ~TEXTURE_CONSTANT_MASK) | constant << FETCH_CONSTANT_INDEX_SHIFT;"),
]

root = pathlib.Path(sys.argv[1])
for path, old, new in BREAKS:
    file = root / path
    text = file.read_text()
    assert text.count(old) == 1, (path, old)
    file.write_text(text.replace(old, new))
print(f"{len(BREAKS)} break in {len(BREAKS)} file")
