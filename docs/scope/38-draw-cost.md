# Milestone 38: the cost of a draw

Agreed with the user on 2026-10-15, as the first step in narrowing what xsdk's GPU layer costs the CPU.

## Why

vkbench's cube scenes on the console, at xsdk `608c29f` and vulkan4xenon `c1e3aaa`:
- **Through the GPU layer:** a cube costs the CPU 0.61 us. `draws_4000` draws and submits in 2,433 us, against 159
  for `draws_250`.
- **Through vulkan4xenon,** which records into the layer: 1.31 us a cube.
- **What a cube writes:** 49 dwords, 196 bytes (vkbench's ring counts). At 0.61 us that's 0.33 GB/s, so most of the
  cost is likely the code that writes them, not the memory. The probe below measures the stores alone, to check.

What the code does for each cube:
- **Constants go in whole groups of 4.** That's the only way the console took them (docs/HARDWARE.md, "Float constants
  go in groups of 4"). Each group is one type-0 packet with up to 3 dwords of alignment either side. So the matrix
  writes 20 dwords, and so does the tint, though it's one vector.
- **Values are read back just after being stored.** `set_floats` copies the values into the stage's shadow, then reads
  the group back out of it to write the packet (src/gpu_constants.c). Every draw and state call builds its packets in
  an array on the stack, then `xsdk_gpu_write` reads them back to copy them into the stream (src/gpu_draw.c,
  src/gpu.c). On an in-order core, a load from an address a store has just written may wait for that store. The probe
  measures whether it does here.
- **Kicks:** the stream is kicked every 1,024 dwords, about every 20 cubes. Each kick is a `sync` and an uncached
  store (src/gpu.c, `submit_segment`).

## What it takes

1. **A probe**, `tests/probe_draw_cost.c`, times vkbench's cube a call at a time:
   - The vertex constants, the pixel constants and the indexed draw, alone and together, recorded into a command
     buffer the GPU never runs. Recording measures the CPU alone, and makes any layout safe to time.
   - 49 dwords stored straight into the buffer, and the same built on the stack and copied, as the layer does.
   - With the GPU open, the cube's constants through the stream, to cost its segments and kicks.
2. **Cut what the probe shows.** The candidates:
   - packets written straight into the space reserved for them, not built on the stack and copied;
   - constants written from the caller's values and the shadow, without reading back what was just stored;
   - less alignment padding, where it costs;
   - fewer kicks.

   Constants stay in whole groups of 4. Whether a group can be smaller would need a console probe that risks a hang,
   so it's a step of its own, taken only if the cuts above fall short.
3. **Probe again, and run vkbench.** vulkan4xenon gains through its rebuild, with no change of its own.

## Done when

- HARDWARE.md records the probe's numbers before and after, from the console.
- vkbench's `draws_4000` draws and submits through xsdk in 1.2 ms or less, from 2.43, about 0.3 us a cube.
  - `states_1000` and `uniforms_1000` come down with it.
  - Both backends still draw the same pictures, and all 14 checks pass.
- xsdk_test passes on the console. Broken on purpose, so constants are written from the shadow before it takes the new
  values, the constant checks fail.
- vulkan4xenon_test passes on the console, and Dusklight plays.

Done on 2026-10-15 (docs/HARDWARE.md, "The cost of a draw, milestone 38"): `draws_4000` draws and submits in 1,162 us,
0.29 us a cube. The probe's findings also put the stream's reservations inline and grew its segments, each within
what the probe showed. Dusklight wasn't run: the user didn't need it played, today's goal being xsdk's improvements.
