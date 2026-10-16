"""Breaks xsdk_test's checks for vulkan4xenon's milestone 20 in a copy of xsdk: breaks.py SET DIR, DIR holding the copy
builds.sh made.

- 2a (the layer's step 2): the vertex element's picks ignored, the vertex test's picked-components check.
"""
import pathlib
import sys

SETS = {
    "2a": [
        ("src/shader.c", "\t\tif (element->components[source])\n", "\t\tif (element->components[source] && false)\n"),
    ],
}


def apply(name, root):
    texts = {}
    for path, old, new in SETS[name]:
        if path not in texts:
            texts[path] = (root / path).read_text()
        assert texts[path].count(old) == 1, (path, old)
        texts[path] = texts[path].replace(old, new)
    for path, text in texts.items():
        (root / path).write_text(text)
    print(f"set {name}: {len(SETS[name])} breaks in {len(texts)} files")


if len(sys.argv) != 3 or sys.argv[1] not in SETS:
    sys.exit(f"breaks.py {'|'.join(SETS)} DIR")
apply(sys.argv[1], pathlib.Path(sys.argv[2]))
