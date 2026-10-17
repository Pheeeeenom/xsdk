# Milestone 45: xost's code quality

Agreed on 2026-10-17.

## Why

Every shader a title draws through xsdk or vulkan4xenon is xost's microcode, so its instruction count and registers
set what a pixel or a vertex costs the GPU. Measured with xost's own disassembler (`xenos-toolchain/xdis`) over
Dusklight's 150 GX shaders and vkbench's 7, compiled at xsdk's register split with the optimiser on, as Dusklight's
pack is built:
- **6641 ALU instructions,** 4930 in the 47 vertex shaders (up to 245 in one) and 1687 in the 103 pixel shaders.
- **2344 of them, 35%, are copies:** a `max` of a value with itself. A component computed into a temporary (a `dp4`
  into `r3.x`) is copied into the vector being built (`max r2.x, r3, r3`). The optimiser propagates copies into their
  readers (`src/xsc_optimize.c`, `propagate_copies`), but not a computed value into its copy's place.
- **583 slots, 9%, co-issue a scalar operation** beside the vector one, though every ALU instruction has room for both
  (xenia:src/xenia/gpu/ucode.h:1854-1878).
- **Registers:** pixel shaders take 6.2 GPRs on average and up to 17, vertex shaders 9.8 and up to 15, each component
  computed in a temporary of its own. `SQ_PROGRAM_CNTL`'s register counts size every thread
  (xenia:src/xenia/gpu/registers.h:134-136), and the more a thread takes, the fewer run to hide fetch latency.
- **Constants built in ALU:** a position's w of 1 is made with a `max` from a literal, where a vertex fetch can give
  0 or 1 in any component itself (`FetchDestinationSwizzle`, xenia:src/xenia/gpu/ucode.h:677-692).
- **Fetches nothing uses:** 5 pixel shaders keep a texture fetch whose result never reaches an export.

A title that wants more from the GPU should get it from xost's code before it gives up pixels. Every step here keeps
the arithmetic as it is, so a program computes the same bits before and after; only where values live and how
instructions pair changes.

## What it takes

Each step is built, then verified as below, then the next begins.

1. **Measurement.** `tools/check_optimize.py`, which already compiles a folder of shaders with and without the
   optimiser and compares their exports on random inputs (`xdis/equivalence.py`), also counts copies, co-issued
   slots, fetches, control flow and GPRs, per shader and in total. The baseline for Dusklight's GX shaders, xost's
   testdata and vulkan4xenon's shaders goes in `results/`. A console probe, `probe_shader_cost`, draws a set of the
   pack's pixel shaders over 1280x720 and a vertex-heavy mesh with the heaviest vertex shaders, timed by the GPU's
   clock, so each step's gain is measured on the hardware as well as counted.
   Built on 2026-10-17: the counts and the baseline are in `results/2026-10-17-milestone-45/`, and the probe times
   every shader in the pack, hashing each pixel shader's picture, with its two baseline runs on the console beside
   them.
2. **Values computed into place.** Where a copy moves a computed component into another register's component, and
   nothing between the computation and the copy reads or writes either place, the computation writes there and the
   copy goes. This covers vectors built a component at a time and exports assembled from parts.
   Built and run on the console on 2026-10-17: the optimiser's `compute_in_place` takes the corpus from 8895 ALU
   instructions to 6887 and from 3253 copies to 1271, and Dusklight's pixel shaders take 15.0% less of the GPU, its
   vertex shaders 14.7%, recorded in `results/2026-10-17-milestone-45/`. It made compiling Dusklight's 150 programs on
   the console 2.6 times slower, 1.32 s to 3.39 s; agreed on 2026-10-17 to bring that back within this milestone.
3. **Registers packed.** The allocator gives values whose components don't overlap in time components of one
   register, rather than a register each.
   Built and run on the console on 2026-10-17: in the programs the optimiser rewrites, the allocator's
   `pack_registers` takes the corpus from 1973 registers to 1529, Dusklight's pixel shaders from 6.2 a shader to 4.5
   and its vertex shaders from 7.5 to 6.3, and its pixel shaders take 4.9% less of the GPU than at step 2, recorded in
   `results/2026-10-17-milestone-45/`.
4. **More co-issue.** With the copies gone, more scalar operations stand next to an independent vector one; the
   pairing takes the vector instruction before or after where it sits now when the order allows.
5. **Constants from the fetch.** A vertex attribute's missing components that the shader fills with 0 or 1 come from
   the fetch's destination swizzle, not ALU.
6. **Fetches nothing uses dropped,** with the exports' liveness the optimiser already tracks.

## Verification, without the user

- **On the PC, every step:** xost's tests; `check_optimize.py` on every shader of the corpus, the step on against off,
  exports equal on random inputs; containers the console has passed come out byte for byte the same unless the step
  targets them; the counts against the baseline.
- **On the console, every step,** driven from the PC through the user's local console agent, which uploads, launches,
  fetches logs and reboots: xsdk_test, whose compiler check compiles shaders on the console and compares them with
  the PC's; vulkan4xenon_test, twice, its shaders compiled with the optimiser by the step's xost; `probe_shader_cost`
  against the baseline, its pictures of every program in Dusklight's pack compared pixel for pixel with the
  baseline's, as the arithmetic doesn't change; and Dusklight's title scene as a smoke run, its 600th frame captured
  and looked at. Break sets as before, each failing the checks it names.
- **Dusklight's frames,** agreed on 2026-10-17: two runs of one build capture the same 600th frame but for the sky,
  but a step that makes shaders cheaper reaches it at another point of the ride, so frames aren't compared across
  steps; the probe's pictures are the pixel check.
- **Recovery:** a GPU hang ends the run, and the agent's reboot recovers it, as for milestone 28's 13 hung tries. The
  user is needed only if the console stops answering the agent, or if xbWatson breaks into an assert, so it stays
  closed during the runs.

## Done when

- Every copy left is one no step can take: a constant, a fetched value into an export, or one in a shader the
  optimiser leaves alone, counted by kind (agreed on 2026-10-17, in place of under 5% of the ALU instructions, as
  copies of constants alone are 5.7% of Dusklight's and vkbench's after step 2). The corpus's ALU total is down by at
  least 25% from the baseline.
- Co-issued slots and GPRs improve on the baseline, with the numbers recorded.
- Every shader's exports match with the optimiser on and off, and every console check above passes.
- Compiling Dusklight's 150 programs on the console, as `probe_xsc` times it, takes about what it did before the
  milestone, 1.32 s.
- `probe_shader_cost` shows the gain on the console, recorded in docs/HARDWARE.md.

## Not in it

- Changing the arithmetic: reassociating, refolding constants, or anything else that changes a result's bits.
- Control flow: loops, branches and predication stay as they are.
- The GLSL itself, such as Aurora's generator, and xsdk's or vulkan4xenon's CPU costs.
