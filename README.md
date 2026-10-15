<div align="center">
  <img src="assets/xsdk.png" alt="xsdk" width="640">
</div>

# xsdk

A from-scratch homebrew SDK for the Xbox 360. It builds `.xex` titles that run on the retail kernel and dashboard of
an RGH console, and its GPU layer drives Xenos directly, writing the command processor's packets itself. Everything
in it comes from public reverse engineering, mainly Xenia, libxenon and xost, and from results on the console: no
Microsoft tooling and no leaked SDK material.

**Status:** milestones 1 to 39 (SCOPE.md, section 4) passed on the console as of 2026-10-15. docs/HARDWARE.md records
each fact the console settled, and TODO.md what's still open.

## What's in it

- **Titles:** `tools/elf2xex.py` packs an ELF into a XEX2 the retail loader accepts, with imports by ordinal. A C17
  runtime on newlib gives threads, files, time, controller input and audio, and the C++23 runtime exceptions, threads,
  atomics, chrono, filesystem and format.
- **The GPU layer** (`include/xsdk/gpu.h`):
  - commands recorded into a stream the ring runs as indirect buffers, so the GPU works while the CPU records;
  - colour and depth targets placed in eDRAM, and resolves to memory and to textures;
  - shaders from xost, loaded with `IM_LOAD`, and their vertex layouts;
  - textures in linear and tiled layouts, mipmaps, samplers and the formats titles need;
  - indexed draws of points, lines, triangle lists, strips and fans, several vertex streams, depth and stencil,
    blending, culling and viewports;
  - shader constants set from the CPU or loaded by the GPU straight from memory;
  - command buffers recorded once and run many times;
  - presenting paced to vblank at 720p, and the vulkan4xenon boot logo (`xsdk_splash_*`).
- **Speed, on the console:** a cube with its own transform and tint takes 0.63 us to draw and submit, and `memcpy`
  copies 5.6 GB/s into write-combined memory (vulkan4xenon's vkbench, docs/HARDWARE.md).

## Building

The toolchain is GCC 15, binutils and newlib for `powerpc-xsdk-elf`, built in WSL Debian by
`tools/toolchain/build.sh` (its specs and patches say what differs from a stock build) and installed at `~/xsdk-gcc`.
Then, in the repository:

```
make
```

That makes `build/libxsdk.a` and the test title, `build/xsdk_test.xex`, as well as the bring-up probes. A title links
its own objects and `libxsdk.a` with `src/xsdk.ld`, which starts it at `xsdk_start` in the library, then packs the ELF
with `tools/elf2xex.py`; vulkan4xenon's Makefile does it that way. CMake projects get the same through
`-DCMAKE_TOOLCHAIN_FILE=cmake/xsdk.cmake` and `xsdk_add_xex(<target>)`; `make cmake_check` builds a title that way.

## Testing

`build/xsdk_test.xex` runs every test in one title and writes a pass or fail line for each to `game:\xsdk_test.log`:
each title's log is named after its `.xex`. Copy it to the console, launch it from the dashboard, and read the log. Xenia Canary serves for quick crash and smoke
checks, but the console decides; CLAUDE.md lists where Xenia differs.

## Layout

- `include/xsdk/` and `src/`: the SDK; `imports/` the kernel and XAM imports it links by ordinal.
- `tests/`: the test title and the probes, titles that each answer one question on the console.
- `tools/`: the XEX packer, the shader and import generators, and the toolchain build.
- `SCOPE.md`: what the SDK covers and its milestones. `CODE_GUIDELINES.md`: how its code is written.
- `docs/HARDWARE.md` and `results/`: what each console run showed, with its logs and captures.

## Related

- [vulkan4xenon](https://github.com/xenosworks/vulkan4xenon), a Vulkan 1.0 subset built on xsdk's GPU layer, on the
  way to running Dusklight.
- [xost](https://github.com/xenosworks/xenos-toolchain), the shader toolchain, checked out in `xenos-toolchain/`.
- [SDL-xenon](https://github.com/xenosworks/SDL-xenon), SDL 3 with an Xbox 360 platform built on xsdk.

## Licence

xsdk is under the MIT licence (LICENSE), copyright Mena. A few files carry code from libxenon and Xenia under the
3-clause BSD licence; THIRD_PARTY_NOTICES.md has their notices and what a game built with xsdk ships to meet them. The
toolchain's patches are under the licences of GCC, binutils and newlib.

## Trademarks

xsdk isn't affiliated with or endorsed by Microsoft. Xbox and Xbox 360 are trademarks of Microsoft. The repository
holds no Microsoft code, binaries, firmware, keys or microcode (CODE_GUIDELINES.md).
