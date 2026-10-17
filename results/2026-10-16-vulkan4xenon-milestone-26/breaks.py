"""vulkan4xenon milestone 26's breaks of xsdk_test, each a small source change that should fail the checks named with it.

breaks.py list prints the sets; breaks.py checks <set> the checks it should fail; breaks.py apply <set> <copy> applies
it to a copy of xsdk. builds.sh builds each set's copy into a .xex of its own. Every other check should pass with each
set.
"""
import pathlib
import sys

GPU_DRAW = "src/gpu_draw.c"

DEPTH_ON = "depth: overlaps, the stencil mask, two-sided stencil and the cleared depth all right with the depth test on"
DEPTH_OFF = "depth: with the depth test off, the depth checks fail and only they do"
DEPTH_WORDS = "depth: the overlaps' depth resolved into a D24S8 texture, each word its depth above its stencil"

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
