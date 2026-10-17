"""Breaks xsdk_test's checks of visibility queries, for vulkan4xenon milestone 28, in a copy of xsdk, a set at a time:
xsdk_test_breaks.py <copy> <set>, or xsdk_test_breaks.py --list.

Each break is a small source change that should fail the checks named beside it, and no others; xsdk_test_builds.sh
builds each set into a title of its own (builds.sh here builds the probe's tries). Xenia sets a query's bit as the end
is read, ignoring PA_SC_VIZ_QUERY, so only the console fails sets a and c.
- a: the status word copied without waiting for the GPU to be idle, which on the console held the bits from before.
- b: every id's bit read from the first status word, ids 32 to 63's too.
- c: the id put in PA_SC_VIZ_QUERY unshifted, over its enable bit: on the console the draws set the register's id's bit,
  not the packet's, so the quad's query reads clear and the reused id reads set.
The status words outlast a title, so the hidden quad's id, and in set c the high ids, may hold a bit from an earlier
run whatever the break: those checks aren't listed where a run can pass them on what it found.
"""
import pathlib
import sys

QUAD = "visibility: a query around a drawn quad sets its bit"
HIDDEN = "visibility: a query around a quad hidden behind another sets its bit too, the conservative answer"
REUSED = "visibility: an id begun again around no draw, after a quad set its bit, comes back clear"
HIGH = "visibility: ids 32 to 63 answer in the second status word, 63's quad set and 32's no draw clear"

# Each set's breaks: the file, the text it changes, what it becomes, and the checks it should fail.
SETS = {
    "a": [
        ("src/gpu_draw.c",
         "gpu_put_register(&packets, WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);\n\tgpu_put_type3(&packets, PM4_REG_TO_MEM",
         "gpu_put_register(&packets, WAIT_UNTIL, 0);\n\tgpu_put_type3(&packets, PM4_REG_TO_MEM", [QUAD, REUSED, HIGH]),
    ],
    "b": [
        ("src/gpu_draw.c", "id < VISIBILITY_IDS_PER_WORD ? PA_SC_VIZ_QUERY_STATUS_0 : PA_SC_VIZ_QUERY_STATUS_1",
         "PA_SC_VIZ_QUERY_STATUS_0", [HIGH]),
    ],
    "c": [
        ("src/gpu_draw.c", "VIZ_QUERY_ENA | id << VIZ_QUERY_ID_SHIFT", "VIZ_QUERY_ENA | id", [QUAD, REUSED]),
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
