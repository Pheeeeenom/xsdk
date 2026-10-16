# Milestone 41: docs and samples

Proposed on 2026-10-15, with docs/PARITY.md, as the second of the gaps it ranks, after milestone 40's fault
handler. Not yet agreed.

## Why

xsdk is meant for other people's titles, and today it explains itself only to the people building it:
- **Its knowledge is a lab notebook.** SCOPE.md is the plan, and docs/HARDWARE.md, 3,236 lines, records each fact in
  the order the console settled it. The rules that keep a title right on the console are in xsdk's CLAUDE.md, which a
  title's own project won't have: volatile 32-bit stores to GPU memory, big-endian data, what Xenia gets wrong.
- **Its only examples are tests,** written to check xsdk, not to teach it.
- **A title's project can't compile a shader.** The tests' shaders come from a generator script each
  (`tools/gen_*_shaders.py`), and xost's glslang is a Windows build nobody else has, untracked
  (xenos-toolchain/README.md, "Use").

Most people will drive xsdk through an AI assistant, knowing little of the 360 themselves. An assistant does well when
the docs give it:
- the steps in order, with the exact commands and what they print;
- the calls in order;
- the rules that break titles silently on the console;
- a project that already works, to start from;
- a way to check its own title from the log, as it can't see the screen.

## What it takes

1. **Shaders in a title's project.**
   - `tools/build_shaders.py` takes GLSL files through glslang to SPIR-V, then through xost's compiler at xsdk's
     register split, optimised. It writes one C header: each container, its literal constants, and where its
     constants and samplers landed. That's what `tools/gen_draw_cost_shaders.py` does for one probe.
   - `xsdk_add_shaders(target files...)` in `cmake/xsdk.cmake` runs it.
   - glslang is the version xost is checked with, 16.3.0, as Khronos's Linux build, run in WSL. xost's tests pass with
     it before the docs name it.
2. **Samples** under `samples/`, each a CMake project built the way a title's would be, with `cmake/xsdk.cmake`,
   `xsdk_add_xex()` and `xsdk_add_shaders()`. Each one:
   - starts with the splash;
   - runs until Back or Xbox Home;
   - is short and commented for a reader;
   - logs every call that fails;
   - checks once what it can, and logs the check as pass or fail. For a picture, that's a few pixels of its first
     resolved frame, read back. These lines are set apart at the end, as the way a title checks itself.

   The samples:
   - **hello:** the log, the display and a controller. The screen's colour follows the sticks and triggers, and A
     rumbles.
   - **triangle:** GLSL shaders through `xsdk_add_shaders`, vertices in GPU memory, a draw, a resolve and a present.
   - **cube:** an indexed, textured cube with depth, its matrix in shader constants every frame, turned by the left
     stick.
   - **sound:** a tone through the kernel's render driver, its pitch on the triggers.
   - **sdl3:** SDL3's gamepad and audio beside xsdk's drawing, as a port would use them. It's built when SDL-xenon is.
   - **template:** the start of a new title. A `CMakeLists.txt`, a `main.c` that opens the display and the GPU and
     clears the screen, and an `AGENTS.md` (which a `CLAUDE.md` imports) with the rules an assistant needs in a title's
     project.

   `make samples` builds them all, as `make cmake_check` builds `tests/cmake`.
3. **Docs**, in `docs/`, written so an assistant can follow them step by step. Each claim names its evidence in
   docs/HARDWARE.md or a source, and each command shows what it prints.
   - **GETTING_STARTED.md:** from a Windows PC and an RGH or JTAG console to the template running on it:
     - WSL Debian;
     - the toolchain, with `tools/toolchain/build.sh`, and how long it takes;
     - glslang;
     - building xsdk and a sample;
     - copying the `.xex` over FTP and launching it;
     - reading its log;
     - Xenia as a smoke check, and where it differs from the console.
   - **GUIDE.md:** the API by task, each task linked to the sample that does it:
     - a title's life: start, the splash, the display, the GPU, the frame loop, closing;
     - memory the GPU reads;
     - shaders and their constants;
     - textures, render targets and resolves;
     - recordings;
     - threads and where they run;
     - files and paths;
     - sockets;
     - input and audio through the kernel.

     For each call, it says what the call leaves set and what undoes it, and from which threads it may be made. A
     section for porting lists the C and POSIX calls xsdk has, and those it refuses.
   - **PITFALLS.md:** one page of what breaks a title on the console and not on a PC. Each pitfall gives its symptom
     and its fix, and where docs/HARDWARE.md has the evidence:
     - big-endian data;
     - plain stores to GPU memory;
     - constants in groups of four;
     - 64 KiB GPU allocations;
     - a mutex locked after it's freed;
     - sleeps that take twice as long;
     - files that share nothing;
     - xost's GLSL subset;
     - Xenia's differences.
   - **README.md** sends a newcomer to GETTING_STARTED.md first.
   - **TODO.md's stale entries** are fixed. Multisampled targets came with milestone 32. And `texture.h` has 17
     formats, 32-bit float among them, where TODO.md counts milestone 16's 13.
4. **A check by someone new.** A subagent starts with none of this conversation, only the repository. Its toolchain
   goes into a `PREFIX` and `WORK` of its own, so xsdk's isn't touched. It follows GETTING_STARTED.md:
   - it builds the toolchain, glslang, xsdk and the samples;
   - it makes a title from the template that does something the samples don't, such as two cubes, one turning on
     each stick.

   Wherever it gets stuck or goes wrong, the docs are fixed, and a fresh subagent starts over, until one gets through.
   Its title then runs on the console.

## Done when

- `make clean`, `make -j32` and `make samples` build everything without warnings.
- **On the console,** each sample runs, logs its checks passing, and shows and plays what its comment says, which the
  user confirms. Broken on purpose, each one fails:
  - hello clearing to a colour other than the one it checks;
  - triangle's vertices left in cached memory without the invalidate;
  - cube with depth testing off;
  - sound's frames never submitted;
  - sdl3's gamepad never opened.
- **The fresh subagent** gets from the docs alone to a title of its own that runs on the console.
- **xsdk_test** passes on the console if anything in `libxsdk.a` changed.

## Not in it

- **Crash reports,** which are milestone 40, and **the log over the network,** a gap of its own in docs/PARITY.md.
- **New xsdk calls** for input, audio or the Guide's notifications. The samples use the raw calls that exist, and
  change when those milestones come.
- **A prebuilt toolchain to download.** It's a release to decide on its own.
- **Vulkan samples:** vulkan4xenon's test title and vkbench are its examples, in its repository.
