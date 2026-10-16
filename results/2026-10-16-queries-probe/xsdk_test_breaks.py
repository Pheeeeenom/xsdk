"""Breaks xsdk_test's three checks of step 6's functions, or restores them: xsdk6_breaks.py apply, or restore.

xsdk_gpu_write_word writes one more than asked; xsdk_gpu_wait_word waits always-true; xsdk_gpu_write_clock copies the
low word in place of the high ones. The last also breaks the fence-wait check, which times waits by the clock.
"""
import pathlib
import sys

FILE = pathlib.Path("/mnt/z/Xbox-360-SDK/src/gpu.c")
BACKUP = pathlib.Path(__file__).with_name("xsdk6_gpu.c")
BREAKS = [
    ("\tpacket[2] = address | SWAP_8IN32;\n\tpacket[3] = value;\n}\n\n// Read through the swap",
     "\tpacket[2] = address | SWAP_8IN32;\n\tpacket[3] = value + 1;\n}\n\n// Read through the swap"),
    ("packet[1] = WAIT_REG_MEM_MEMORY | WAIT_REG_MEM_EQUAL;", "packet[1] = WAIT_REG_MEM_MEMORY | 7;"),
    ("\t\tRBBM_PERFCOUNTER1_HI,\n\t\tRBBM_PERFCOUNTER1_LOW,\n\t\tRBBM_PERFCOUNTER1_HI,\n",
     "\t\tRBBM_PERFCOUNTER1_LOW,\n\t\tRBBM_PERFCOUNTER1_LOW,\n\t\tRBBM_PERFCOUNTER1_LOW,\n"),
]

if sys.argv[1] == "apply":
    text = FILE.read_text()
    BACKUP.write_text(text)
    for old, new in BREAKS:
        assert text.count(old) == 1, old
        text = text.replace(old, new)
    FILE.write_text(text)
    print(f"{len(BREAKS)} breaks")
else:
    FILE.write_text(BACKUP.read_text())
    BACKUP.unlink()
    print("restored")
