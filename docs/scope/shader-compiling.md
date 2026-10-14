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

The part of xost that turns SPIR-V into a container, C17 as the guidelines have console code: `xsc/` and `xasm/`'s
program, optimiser and encoder, about 6,800 lines of Python, and the tables it takes from `xdis/`. It's built for the
console and for the PC, where it builds the offline packs. One compiler: once the C one gives the same bytes as the
Python one for every shader xost's tests lower and every program in Dusklight's pack, the Python one is retired.
xost's tests, its simulator among them, stay in Python and drive the C compiler. xost's compiler changes wait while it
is ported.

- xost recurses deeply (its tests raise Python's limit to 20,000); in C that recursion is taken out or the threads
  that compile get stacks big enough, measured on the console.
- A C tool on the PC is a change to CODE_GUIDELINES.md, section 2 ("Host tools are Python"), made with this step.

### 3. Compiling as the game asks

- vulkan4xenon compiles SPIR-V it holds no container for, at `vkCreateShaderModule`, through step 2's compiler: the
  runtime SPIR-V compilation its ROADMAP leaves out (section 6).
- Aurora's vulkan4xenon backend compiles GLSL its pack doesn't hold through step 1's glslang.
- Both keep what they compile on the hard disk, by hash, and look there before compiling. A pack becomes a head start,
  not a requirement.
- The compiling happens on Aurora's pipeline threads (aurora-xenon:lib/vulkan4xenon/backend.cpp:49), off the frame;
  a draw whose program is still compiling is skipped, as one missing from the pack is now.

Done when a console with no pack and nothing kept plays Dusklight's title scene and opening cutscenes, and makes a
memory card, drawn: each program missed only the first time it's asked for, as the user sees on screen, and a second
run compiles nothing.

## Left out

Shaders other than through vulkan4xenon and Aurora, and changing what the GLSL generator writes.
