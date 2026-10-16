"""Breaks milestone 19 step 3's checks, or restores them: step3_breaks.py apply a|b|xsdk, or restore.

Set a breaks a check each of the layer's: the stencil, depth bias and blend groups, the refusals and the names. Set b
breaks the pipelines' making, as dynamic blend constants are refused, and with it the frames. Set xsdk writes the
blend constants alpha first.
"""
import pathlib
import sys

BACKUP = pathlib.Path(__file__).with_name("step3_backup")
LAYER = pathlib.Path("/mnt/z/vulkan4xenon")
XSDK = pathlib.Path("/mnt/z/Xbox-360-SDK")

SETS = {
    "a": [
        # The write mask a command sets is ignored: the pipeline's own, 1, writes.
        (LAYER / "src/pipeline_state.c",
         "\tif (takes(pipeline, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK))\n\t\tsetting.stencil_write_mask = dynamic->stencil_write_mask;\n",
         ""),
        # The depth bias a command sets is ignored.
        (LAYER / "src/pipeline_state.c",
         "if (!takes(pipeline, VK_DYNAMIC_STATE_DEPTH_BIAS) || !pipeline->depth_bias_enabled)",
         "if (pipeline || !pipeline->depth_bias_enabled)"),
        # The pipeline's own blend constants stand, dynamic or not.
        (LAYER / "src/pipeline_state.c",
         "return takes(pipeline, VK_DYNAMIC_STATE_BLEND_CONSTANTS) ? dynamic->blend_constants : pipeline->blend_constants;",
         "return pipeline || !dynamic ? pipeline->blend_constants : dynamic->blend_constants;"),
        # Stencil set for one face is recorded as for both.
        (LAYER / "src/command_buffer.c", "\tif (faceMask != VK_STENCIL_FACE_FRONT_AND_BACK) {", "\tif (!faceMask) {"),
        # vkCmdSetBlendConstants isn't found by name.
        (LAYER / "src/proc_addr.c", "\tENTRY(vkCmdSetBlendConstants, SCOPE_DEVICE),\n", ""),
    ],
    "b": [
        (LAYER / "src/pipeline_state.c", "state == VK_DYNAMIC_STATE_DEPTH_BOUNDS)",
         "state == VK_DYNAMIC_STATE_DEPTH_BOUNDS || state == VK_DYNAMIC_STATE_BLEND_CONSTANTS)"),
    ],
    "xsdk": [
        (XSDK / "src/gpu_draw.c",
         "const uint32_t constants[] = { float_bits(rgba[0]), float_bits(rgba[1]), float_bits(rgba[2]), float_bits(rgba[3]) };",
         "const uint32_t constants[] = { float_bits(rgba[3]), float_bits(rgba[2]), float_bits(rgba[1]), float_bits(rgba[0]) };"),
    ],
}


def backup_name(path):
    return str(path).replace("/", "_")


def apply(name):
    BACKUP.mkdir(exist_ok=True)
    texts = {}
    for path, old, new in SETS[name]:
        if path not in texts:
            texts[path] = path.read_text()
            (BACKUP / backup_name(path)).write_text(texts[path])
        assert texts[path].count(old) == 1, (path, old)
        texts[path] = texts[path].replace(old, new)
    for path, text in texts.items():
        path.write_text(text)
    (BACKUP / "applied").write_text("\n".join(str(p) for p in texts))
    print(f"set {name}: {len(SETS[name])} breaks in {len(texts)} files")


def restore():
    for path in (BACKUP / "applied").read_text().split("\n"):
        pathlib.Path(path).write_text((BACKUP / backup_name(path)).read_text())
    (BACKUP / "applied").unlink()
    print("restored")


if sys.argv[1] == "apply":
    apply(sys.argv[2])
else:
    restore()
