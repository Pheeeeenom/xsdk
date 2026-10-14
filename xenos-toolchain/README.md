<div align="center">
  <img src="assets/xost.png" alt="xost" width="640">
</div>

# xost, the Xenos shader toolchain

xost compiles shaders for the Xbox 360's GPU, Xenos, with no Microsoft tooling: GLSL through glslang to SPIR-V, then
SPIR-V to a Xenos shader container, every byte from open code. It's part of xsdk. The SDK's own generators, its
tests' shaders, vulkan4xenon's shaders and Dusklight's shader pack are built with it, and the SDK's titles run them on
the console.

## What's here

- **`xsc/`, the compiler.** Vulkan and OpenGL SPIR-V to a Xenos container: `lower.py` lowers each function,
  `resolve.py` lays out uniform blocks, push constants and samplers, `unroll.py` unrolls loops, and `templates.py`
  holds the instruction templates it fills in, from Xenia's definitions. What it handles and refuses is in
  [`docs/xsc.md`](docs/xsc.md).
- **`xasm/`, the assembler.** The instruction encoders (`encode.py`), the program builder (`program.py`: execs, fetch
  layout, register packing, container metadata), the optimiser (`optimize.py`), the text assembler (`textasm.py`,
  [`docs/textasm.md`](docs/textasm.md)) and an N64 colour-combiner compiler (`combiner.py`).
- **`xdis/`, the disassembler and simulator.** The microcode decoder, its ISA tables transcribed from Xenia (BSD-3,
  see `xdis/xenos_ucode.py`), the container parser and emitter ([`docs/container.md`](docs/container.md)), and
  `simulate.py`, which runs a program's ALU, fetches and exports on the CPU.
- **`xcomb/`, the combiner compiler in C99**, byte for byte the same as `xasm/combiner.py`.
- **`tests/`**, run one file at a time: `python tests/test_xsc_lower.py`.

## Use

On the PC: Python 3.11 or later, standard library only. glslang goes in `tools/glslang` and the reference clones the
docs cite, Xenia, libxenon, XenosRecomp and mupen64-360, in `reference/`; neither is tracked.

```sh
# Lower a SPIR-V module to a container, laid out for xsdk's register split
python -m xsc.lower shader.spv -o shader.bin --pixel-registers 64 --vertex-registers 64 --optimize

# Disassemble a container; --exact gives text that reassembles byte for byte
python xdis/xdis.py shader.bin
```

vulkan4xenon's `tools/build_shaders.py` and Aurora's `tools/build_glsl_pack.py` take this folder as `--xost`.

## History

xost was a repository of its own until 2026-10-14, when it joined xsdk:
[github.com/xenosworks/xenos-toolchain](https://github.com/xenosworks/xenos-toolchain), last commit `4a785c6`. xsdk's
docs cite xost's commits by that repository's hashes. It keeps what didn't come over:

- the test vectors the first decoders and templates were checked against, shaders compiled with Microsoft's compiler
  that xsdk doesn't carry, and the tests and examples built on them;
- the libxenon demo apps and their built binaries;
- the plans and notes from xost's time as a project of its own.
