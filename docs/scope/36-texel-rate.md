# Milestone 36: the texel rate

Proposed on 2026-10-14; not yet agreed with the user.

## Why

A pixel shader taking four bilinear samples a texel apart, its coordinates worked out in the shader from a constant,
samples 4.1 to 4.5 Gtexel/s through xsdk with xost's register packing: 8.2 to 9.0 a clock of the published 16.
Milestone 31's probe, its four fetches together in one exec and reading an interpolator, sampled 15.9 a clock with 5
registers, and 12.7 with 12 under xsdk's 64 pixel registers (docs/HARDWARE.md, "The pixel rate, milestone 31"). xost's
build holds 6.

- **Where xost puts the work.** xost lays a shader out as it walks each output's expression: a sample's coordinate
  work, then its fetch (xost:xsc/lower.py:2190).
- **How packing reuses registers.** Its register packing frees a coordinate's register at the fetch that reads it
  (xost:xasm/program.py:393-475). So the three coordinates share one register, and each add waits for the fetch before
  it.
- **What the hardware is told.** An exec's sequence bits mark each instruction as ALU or fetch, and whether it waits
  for the fetches before it (xenia:src/xenia/gpu/ucode.h:230-231). xost sets only the first (xost:xasm/program.py:305).
- **Dusklight's shaders.** Lowered as its pack is, Dusklight's pixel shaders read 42 of their 159 samples at
  coordinates an ALU instruction wrote, in 29 of 98 programs; 27 of those put ALU work between their fetches. 25
  programs hold 10 to 17 registers.

## What it takes

- **A probe** times hand-built pixel shaders taking four samples at coordinates each works out, one layout a variant:
  - as xost lays it out now;
  - each coordinate in a register of its own;
  - the coordinates first, and the fetches together after them;
  - two coordinates to a register;
  - each with and without the wait bit on the first instruction reading a sample.

  Then the best of them at 12 and 17 registers, under xsdk's split and under one giving pixel shaders more, and a
  trilinear 64-bit texture, whose rate fell from 0.85 to 0.55 Gtexel/s with xost's packing.
- **xost lays out what the probe finds fastest:** within a block, the work a fetch's coordinates need ahead of a run of
  fetches, and their registers kept until the run ends.

## Done when

- HARDWARE.md records each layout's rate on the console, and which reach 16 a clock.
- xost's build of the four-sample shader, timed by the probe, runs within 5% of the fastest hand-built layout. Broken
  on purpose, xost laying it out as now, it falls back on the console.
- xost's tests check the layout, and shaders that sample only at interpolated coordinates lower as before.
- Dusklight's shader pack, rebuilt, draws the logo shot as before, its GPU time measured.
- xsdk_test, vulkan4xenon_test and vkbench pass on the console.

## Left for later

Changing xsdk's split, which shrinks vertex shaders' share as it grows pixel shaders', and the trilinear 64-bit rate
wait on the probe's numbers, each a milestone of its own if those call for one.
