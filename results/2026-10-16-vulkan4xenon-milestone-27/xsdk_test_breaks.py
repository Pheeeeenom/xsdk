"""Breaks xsdk_test's checks of resolving part of a target, for vulkan4xenon milestone 27, in a copy of xsdk, a set at a
time: xsdk_test_breaks.py <copy> <set>, or xsdk_test_breaks.py --list.

Each break is a small source change that should fail the checks named beside it, and no others; builds.sh builds each
set into a title of its own. The sets fail the same checks, so each has a title of its own.
- a: the resolve's rectangle at the target's corner, its scissor still at the rectangle.
- b: the whole target resolved and cleared, whatever rectangle is asked for.
- c: a rectangle's depth clear leaving hierarchical depth on, as the whole's clear left it; Xenia has no hierarchical
  depth, so only the console fails it.
"""
import pathlib
import sys

COPIED = ("resolve rect: rectangles inside a target and at its edges, off the 8-pixel grid there, resolve into the same "
          "texels and no others")
CLEARED = "resolve rect: each rectangle's resolve clears it alone"
DEPTH = "resolve rect: a rectangle's depth clear clears its depth alone, and draws find it"

# Each set's breaks: the file, the text it changes, what it becomes, and the checks it should fail.
SETS = {
    "a": [
        ("src/gpu_resolve.c", "xsdk_gpu_rectangle(area->x, area->y, area->width, area->height);",
         "xsdk_gpu_rectangle(0, 0, area->width, area->height);", [COPIED, CLEARED, DEPTH]),
    ],
    "b": [
        ("src/gpu_resolve.c", "xsdk_gpu_rectangle(area->x, area->y, area->width, area->height);",
         "xsdk_gpu_rectangle(0, 0, width, height);", [COPIED, CLEARED, DEPTH]),
        ("src/gpu_resolve.c", "\tput_window(&packets, area);", "\tput_window(&packets, &(Area){ 0, 0, width, height });",
         [COPIED, CLEARED, DEPTH]),
    ],
    "c": [
        ("src/gpu_resolve.c", "\telse if (clears_depth)\n\t\txsdk_gpu_hiz_drop();\n", "", [DEPTH]),
    ],
}


def apply(root, name):
    texts = {}
    for path, old, new, checks in SETS[name]:
        file = root / path
        text = texts.get(file, file.read_text())
        assert text.count(old) == 1, (path, old)
        texts[file] = text.replace(old, new)
        print(f"{name}: {path}: should fail {'; '.join(checks)}")
    for file, text in texts.items():
        file.write_text(text)


if sys.argv[1] == "--list":
    print(" ".join(SETS))
else:
    apply(pathlib.Path(sys.argv[1]), sys.argv[2])
