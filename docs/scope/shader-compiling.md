# Compiling shaders on the console

Agreed with the user on 2026-10-14, after milestone 36: the plan in outline, the probe first. A scope of its own, not a
numbered milestone; each step's details are agreed before it starts.

## Why

Dusklight's pack holds only the programs its recordings reached, 145, from the boot screens up to the title scene
(dusklight-xenon:platforms/xenon/shaders). Aurora writes a program for each GX configuration the game sets, so a scene
the recordings never reached asks for programs the pack doesn't hold, and those draws are skipped
(aurora-xenon:lib/vulkan4xenon/pipeline.cpp:55-67). Filling the pack from the PC means someone plays each stretch of
the game drawn badly, the programs are compiled on the PC, and the next build draws them, again and again until a
complete pack turns up and gets shared. Compiling on the console ends that: a program the pack doesn't hold is
compiled the first time the game asks for it, and kept.

The user, 2026-10-14: compiling on the console may be slower than on the PC, and glslang may run there.

## What it takes

Aurora writes GLSL; glslang compiles it to SPIR-V; xost compiles the SPIR-V to a Xenos container, which vulkan4xenon
finds at `vkCreateShaderModule` (docs/ROADMAP.md in vulkan4xenon, section 5). Today the last two run on the PC only:
glslang 16.3.0, a Windows build kept beside xost (`tools/glslang`, not tracked), and xost in Python, which the console
can't run.

On the PC, over Dusklight's 145 programs, glslang took 15.1 s with each run's start, and xost 17.7 s, 122 ms a program
on average and 1.1 s for the slowest, `e5ea8b62e2ebf4ac.vert`.

## Three steps, each run on the console

### 1. glslang on the console (the probe)

glslang built for xsdk with the toolchain, as a library a title links, and a probe title that compiles every program
in Dusklight's pack from its GLSL. It logs each program's time, the slowest, the heap it took at most, and whether its
SPIR-V is the same as the PC's glslang makes. HARDWARE.md records them. glslang's port lives as Mbed TLS's does, in a
fork of its own.

Done on 2026-10-14 (docs/HARDWARE.md, "glslang on the console"; glslang-xenon): all 145 programs came out as on the PC,
the first in 0.94 s with glslang's built-in symbols and the rest in 11.3 ms each on average, 30 ms at most, with
13.5 MB of heap in use at most and 68 KB of stack.

### 2. xost's compiler in C

Agreed with the user on 2026-10-14: the PC builds it in WSL only, the guidelines change, and it lives in libxsdk.

**What's ported.** The path from SPIR-V to a container, with everything vulkan4xenon takes from it
(vulkan4xenon:tools/build_shaders.py:52-71): the container, the literal constants xost pools, and where the uniform
blocks, push constants and samplers landed. In Python that's 5,780 lines, plus the opcode tables and enums it takes
from `xost:xdis/xenos_ucode.py`:
- `xsc/`: reading SPIR-V, laying out the interface, unrolling loops, lowering, and the instruction templates;
- what the compiler uses of `xasm/`: the program builder's stages, exec layout, fetch layout and register packing,
  the optimiser and the encoder;
- `xdis/container_emit.py`.

**What stays in Python.** xost's assembler for shaders written by hand: the program builder and encoder as xsdk's
generators use them (tools/gen_*.py), the text assembler and the combiner. Also the disassembler, the simulator and
the tests. The builder's parts only the compiler uses leave Python with it: the paths for xsc's programs, their fetch
layout and the optimiser. So the encoder and the builder's exec layout exist in both languages. Their output is the
ISA's encoding, which doesn't change, and both are held to the same tests.

**Where it lives.** In xsdk's library, `src/xsc_*.c` with `include/xsdk/xsc.h`, every exported name starting
`xsdk_xsc_` (CODE_GUIDELINES.md, section 4). It's plain C17 that calls only the C library's allocation, never the
kernel, so the same sources build for the PC. A title calls one function that takes SPIR-V and the register split and
gives back the container, the literals and the resources. If xost refuses the shader, it gives the error xost gives
today. A second function frees what the first made.

**On the PC.** The compiler builds there as a shared library. `xsc.lower.lower_ex` and `xsc.resolve.resolve_interface`
keep their Python signatures and call it through `ctypes`, so vulkan4xenon's and Aurora's tools and xost's tests run
unchanged. Two ways to build it:
WSL's gcc builds it, as the SDK's toolchain is built, and the tools that compile shaders run in WSL's Python. A C
tool on the PC changes CODE_GUIDELINES.md section 2, which said host tools are Python; that change comes first, in a
commit of its own.

**How it's checked.**
- **Part by part.** Each part is ported bottom up and checked against Python's before the next starts:
  1. reading SPIR-V;
  2. the encoder, on `xost:tests/test_encode_fuzz.py`'s cases;
  3. the container emitter;
  4. the program builder: the same instructions in, the same microcode out;
  5. the lowering.
- **Byte for byte, before Python's compiler goes.** The C and Python compilers both compile every SPIR-V module in xost's
  testdata, vulkan4xenon's shaders and Dusklight's pack, with and without the optimiser. They must give the same
  containers, literals and resources, and refuse the same shaders with the same kind of error.
- **Order.** Where Python's output depends on the order of a set or a dict, C reproduces it. Where that isn't
  practical, the Python gets a defined order first, and its output is checked on the console before the port goes on.
- **xost's tests** pass against the C compiler. Each check that ported code changes is broken on purpose once and
  must fail.
- **On the console,** a probe title compiles the SPIR-V of every program in Dusklight's pack with the C compiler and
  compares each container with the pack's. It logs each program's time, the slowest, and the heap and stack at most,
  as step 1's probe does. Built with the fetch layout turned off, it must fail.

**Stack.** Across Dusklight's 145 programs, the Python compiler's deepest call chain is 73 frames. The C keeps
recursion where the Python has it. The console probe measures the stack, and step 3 sizes its threads from that.

**While it's ported,** xost's compiler doesn't change, so the two can be compared byte for byte throughout.

**Done when:**
- the C compiler matches the Python one, as above, on the PC;
- the Python compiler is gone, and xost's tests pass through its wrappers;
- the console probe compiles all 145 of Dusklight's programs into the pack's containers, broken on purpose it fails,
  and HARDWARE.md records its numbers;
- vulkan4xenon's shaders and Dusklight's pack, rebuilt through the C compiler, are byte-identical to before.

Calling the compiler from vulkan4xenon and Aurora is step 3.

Done on 2026-10-14 (docs/HARDWARE.md, "xost's compiler in C on the console"; xsdk `02dede7`, `1d86a33`). The port was
written whole and then checked, not part by part: the encoder and the interface on their own, the rest end to end,
1068 compilations byte for byte the same as the Python's. On the console all 145 programs came out as the pack's, in
9.1 ms each on average and 76 ms at most, with the heap at 1.06 MB at most and 30 KB of stack; with the fetch layout
off, 117 did and the probe failed.

### 3. Compiling as the game asks

Agreed with the user on 2026-10-14: vulkan4xenon keeps what it compiles in a folder the app names; the compiler's
version is a hash of its sources; xsdk gets `rename` first, so each file kept is written aside and put in place whole;
and the optimiser is on. vulkan4xenon's milestone 13 (its docs/ROADMAP.md).

**Today:**
- vulkan4xenon finds a shader's container in the app's bundle by its SPIR-V's hash and size. For SPIR-V it holds none
  for, it logs the hash, and `vkCreateShaderModule` returns `VK_ERROR_FEATURE_NOT_PRESENT`
  (vulkan4xenon:src/shader.c:34-61). A pipeline points at the bundle's shader for its life (src/pipeline.c:85-87).
- Aurora's backend finds each program's SPIR-V in its pack by the program's hash, XXH3-64 of its GLSL. It logs a
  program the pack doesn't hold once, and doesn't make that pipeline, so its draws are skipped
  (aurora-xenon:lib/vulkan4xenon/pipeline.cpp:54-68). It makes pipelines one at a time, under a lock of its own
  (pipeline.cpp:35-36), on Aurora's pipeline compilation threads: two on the console (lib/gfx/pipeline_cache.cpp:1152,
  and Dusklight's log), each with xsdk's default 1 MB stack (docs/HARDWARE.md, "Threads' stacks"). glslang took 68 KB of
  stack at most, and xost 30 KB.
- Dusklight keeps its files in `game:\TwilitRealm\Dusklight\`, SDL's pref path, and hands Aurora that as its cache path
  (dusklight-xenon:src/m_Do/m_Do_main.cpp:693-697).
- xsdk's `rename` fails with `ENOSYS` (TODO.md), so a file can't be written aside and put in place whole.
- Aurora's pipeline cache database doesn't open on the console ("Failed to open pipeline cache database: out of
  memory", in Dusklight's log), so Aurora doesn't make the pipelines it knew at start.

**In five parts,** each run on the console before the next:

1. **xsdk's `rename`,** over `NtSetInformationFile`'s rename class, replacing what's at the new name as POSIX's does,
   which `std::filesystem::rename` then takes too (TODO.md). The class wants 16 bytes
   (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc:170-172); their layout, whether to replace, a root directory
   and the new name, is the original Xbox kernel's, unverified on the 360 until the console runs it. Xenia takes the
   class and renames nothing (its handler has no case for it, xboxkrnl_io_info.cc:202-251), so the check fails there.
   xsdk_test checks a file renamed over another and a directory renamed, and Dusklight saves its settings again.
2. **vulkan4xenon compiles what its bundle doesn't hold.** At `vkCreateShaderModule`, SPIR-V the bundle has no
   container for goes through `xsdk_xsc_compile`, at xsdk's register split with the optimiser on, as Dusklight's pack
   is built. The container, literals and resources become a shader the layer keeps, by its SPIR-V's hash and size,
   until the device goes: pipelines point at it as at the bundle's, and a second module from the same SPIR-V doesn't
   compile again. A lock guards the shaders kept, as Vulkan lets modules be made on several threads. A shader xost
   refuses fails as a missing one does now, logged with xost's message. The ROADMAP's section 6 stops leaving runtime
   SPIR-V compilation out. vulkan4xenon_test checks that a shader its bundle doesn't hold, compiled on the console,
   draws what the same shader built offline draws.
3. **vulkan4xenon keeps them on the hard disk.** An app names a folder with a call vulkan4xenon.h adds,
   `v4x_set_shader_cache`; without one, nothing is kept. Each shader compiled is a file there, named by its SPIR-V's
   hash and size, in a folder named after the compiler's version, `xsdk_xsc_version()`: a hash of the compiler's
   sources that make works out, so an xsdk whose compiler changed compiles afresh rather than read what an older one
   made. The layer looks there before compiling, and writes each file beside its name and renames it into place, so a
   crash leaves none half written. vulkan4xenon_test checks that a shader compiled is read back from its file.
4. **Aurora's backend compiles GLSL its pack doesn't hold,** through glslang-xenon with step 1's settings, which give
   the PC's SPIR-V (glslang-xenon:xsdk/glslang_probe.cpp), on the pipeline compilation thread that asked. glslang
   starts at the first program it compiles, in 0.94 s and with 13.5 MB of heap it keeps (docs/HARDWARE.md, "glslang on
   the console"), so a run that finds every program doesn't pay for it. The SPIR-V is kept under Aurora's cache path,
   a file a program named by its hash, in a folder named after glslang's version, and read from there before compiling.
   The backend names the folder beside it to vulkan4xenon (part 3's call). It logs each program it compiles with the
   time it took, and how many programs it compiled and read in the run. glslang-xenon builds within Dusklight's build,
   from a checkout beside it, found as vulkan4xenon is (`GLSLANG_XENON_DIR`, as `VULKAN4XENON_DIR`,
   dusklight-xenon:CMakeLists.txt:87).
5. **Dusklight builds without its pack when asked** (`DUSKLIGHT_GLSL_PACK=OFF`), for the run that starts with nothing.
   The pack stays in by default, as a head start.

**Done when:**
- a console with no pack and nothing kept plays Dusklight's title scene and opening cutscenes, and makes a memory card,
  drawn: each program missed only the first time it's asked for, as the user sees on screen, and a second run compiles
  nothing, its log showing every program read from the disk. The user plays both runs;
- with its pack, Dusklight's title scene compiles nothing and runs as at step 2: its frame rate, and the logo shot's GPU
  time and end frame;
- vulkan4xenon_test, vkbench and xsdk_test pass as before;
- HARDWARE.md records both runs: the programs compiled and read, the times, the slowest, the first with glslang
  starting, and the heap.

**Left out of step 3:** compiling before the game asks, and compiling on several threads at once, as Aurora's backend
makes pipelines one at a time; Aurora's pipeline cache database; Vulkan's pipeline caches; giving
glslang's memory back once nothing's left to compile.

## Left out

Shaders other than through vulkan4xenon and Aurora, and changing what the GLSL generator writes.
