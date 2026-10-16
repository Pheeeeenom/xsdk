"""Breaks vulkan4xenon milestone 21's xost on purpose, in a copy of xsdk, never the tree itself.

breaks.py list                 the breaks' names
breaks.py apply NAME ROOT      applies one to the copy of xsdk at ROOT
breaks.py fails NAME           xsdk_test's checks it should fail

builds.sh builds xsdk_test once for each, its tests/xsc_shaders.h left as the PC's compiler made it unbroken, so each
check of a shader whose container, literals or resources the break changes fails, and the others pass. vulkan4xenon's
results/2026-10-16-milestone-21/breaks.py takes the same breaks for vulkan4xenon_test's checks.
"""
import pathlib
import sys

# Each break: its edits, (path in xsdk, text, the text in its place), each text found exactly once.
BREAKS = {
    # A select of an if/else on data takes the other arm: the cnd's arms trade places.
    "select_swap": [
        ("src/xsc_lower_flow.c",
         "\tconst XscOperand first = swap ? fv : tv;\n\tconst XscOperand second = swap ? tv : fv;\n",
         "\tconst XscOperand first = swap ? tv : fv;\n\tconst XscOperand second = swap ? fv : tv;\n"),
    ],
    # The arms in execs on the predicate run under each other's: (p0) takes the false arm and (!p0) the true one.
    "predicate_inverted": [
        ("src/xsc_lower_flow.c", "arm ? XSC_PREDICATE_FALSE : XSC_PREDICATE_TRUE",
         "arm ? XSC_PREDICATE_TRUE : XSC_PREDICATE_FALSE"),
    ],
    # A hardware loop's counter stays 0: each pass multiplies it by 1 rather than adding 1.
    "counter_stuck": [
        ("src/xsc_lower_flow.c", "xsc_emit2(l, XSC_VECTOR_ADD, xsc_temp(counter),",
         "xsc_emit2(l, XSC_VECTOR_MUL, xsc_temp(counter),"),
    ],
    # What a hardware loop carries keeps its value on entry: the copies at a pass's end are left out.
    "carried_kept": [
        ("src/xsc_lower_flow.c", "if (!same && xsdk_xsc_emit_copy(l, next[i], regs[i],",
         "if (false && !same && xsdk_xsc_emit_copy(l, next[i], regs[i],"),
    ],
    # A pixel shader's loops take loop constants from 0, as a vertex shader's do.
    "pixel_loops_at_0": [
        ("src/xsc_lower_flow.c", "\tPIXEL_FIRST_LOOP = 16,", "\tPIXEL_FIRST_LOOP = 0,"),
    ],
    # A Phi where an if/else's arms meet takes each arm's value from the other.
    "phi_swapped": [
        ("src/xsc_unroll.c", "const uint32_t operands[3] = { at->condition, values[0], values[1] };",
         "const uint32_t operands[3] = { at->condition, values[1], values[0] };"),
    ],
}

# xsdk_test's checks (tests/test_xsc.c) each break fails, found on the PC by compiling tests/xsc_shaders.h's shaders
# with the break in and comparing.
CHECK = "xsc: {} compiles into the PC's container, literals and resources"
FAILS = {
    "select_swap": ["vk_vertex_if.vert", "vk_loop_count_if.frag"],
    "predicate_inverted": ["vk_vertex_uniform_if.vert"],
    "counter_stuck": ["vk_loop_count_if.frag"],
    "carried_kept": ["vk_loop_count_if.frag"],
    "pixel_loops_at_0": ["vk_loop_count_if.frag"],
    "phi_swapped": ["vk_vertex_if.vert"],
}


def apply(name, root):
    texts = {}
    for path, old, new in BREAKS[name]:
        file = root / path
        text = texts.get(file, file.read_text())
        assert text.count(old) == 1, (path, old)
        texts[file] = text.replace(old, new)
    for file, text in texts.items():
        file.write_text(text)
    print(f"{name}: {len(BREAKS[name])} edits in {len(texts)} files of {root}")


def main():
    if sys.argv[1] == "list":
        print("\n".join(BREAKS))
    elif sys.argv[1] == "apply":
        apply(sys.argv[2], pathlib.Path(sys.argv[3]))
    elif sys.argv[1] == "fails":
        print("\n".join(CHECK.format(shader) for shader in FAILS[sys.argv[2]]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
