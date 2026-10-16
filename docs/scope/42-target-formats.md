# Milestone 42: render target formats

Proposed on 2026-10-15, as the first of three milestones toward HDR and deferred rendering; multiple render targets
and predicated tiling follow it. Not yet agreed.

## Why

Every colour target xsdk draws into is `k_8_8_8_8`. `xsdk_gpu_put_surface` writes RB_COLOR_INFO as the target's tile
alone, so its format field is 0 (`src/gpu_draw.c`). Resolves copy into `k_8_8_8_8` only (`src/gpu_resolve.c`).
- **What's missing.** No HDR: nothing above 1.0, nothing below 0. No 10-bit colour, and no float buffers such as
  shadow-map depth in `R32F` or velocity in `R16G16F`.
- **What the hardware has.** Xenos renders into 10 distinct colour formats: `ColorRenderTargetFormat`,
  xenia:src/xenia/gpu/xenos.h:299-319, set in RB_COLOR_INFO's `color_format`, registers.h:736-750. 7e3, the
  console's own 10-bit float, is the format its games used for HDR.
- **Who needs it.** PC games and vulkan4xenon's ports ask for float attachments, and Dusklight's renderer is the kind
  that would use them.
- **What the formats change.**
  - 64-bit formats take twice the eDRAM tiles across: `GetSurfacePitchTiles`, xenos.h:426-437, and the base offsets
    in draw_util.cc:1132-1138.
  - A resolve converts the target's format into the destination's: RB_COPY_DEST_INFO's `copy_dest_format`,
    registers.h:870-884. Xenia emulates that conversion in xenia:src/xenia/gpu/shaders/resolve.xesli.

## What it takes

1. **What the console settles first,** each recorded in docs/HARDWARE.md:
   - which half of a 64-bit clear goes in RB_COLOR_CLEAR and which in RB_COLOR_CLEAR_LO. Xenia hasn't checked
     (xenia:src/xenia/gpu/draw_util.h:565);
   - what `k_16_16` and `k_16_16_16_16` hold: Xenia's notes give fixed point from -32 to 32 (xenos.h:306-310);
   - which formats blend;
   - which take 2 and 4 samples;
   - which conversions a resolve makes, 7e3 into half floats in particular.
2. **Targets in each format.** A colour format the target calls take, for every target set after it, as
   `xsdk_gpu_set_resolve_clears` works for resolves. The default stays `k_8_8_8_8`. The tile counts
   (`xsdk_gpu_target_tiles` and the rest) double for 64-bit formats.

   Formats, with the texture each resolves into:

   | Target | Resolves into |
   |---|---|
   | `k_8_8_8_8` | as today |
   | `k_8_8_8_8_GAMMA` | `k_8_8_8_8`, sampled with the fetch constant's gamma sign (xenos.h:121-129) |
   | `k_2_10_10_10` | `XSDK_FORMAT_A2B10G10R10` |
   | `k_2_10_10_10_FLOAT` (7e3) | `XSDK_FORMAT_R16G16B16A16F`, if the console converts it |
   | `k_16_16`, `k_16_16_16_16` | new 16-bit fixed-point textures |
   | `k_16_16_FLOAT`, `k_16_16_16_16_FLOAT` | `XSDK_FORMAT_R16G16F`, `XSDK_FORMAT_R16G16B16A16F` |
   | `k_32_FLOAT` | `XSDK_FORMAT_R32F` |
   | `k_32_32_FLOAT` | a new `R32G32F` texture |
3. **Resolves and clears.**
   - `xsdk_gpu_resolve_texture` resolves into the texture's own format, refusing a pair the table doesn't list.
   - A clear colour given in floats is packed for the target's format, both halves for 64-bit ones.
4. **The test**, `tests/test_target_formats.c` in xsdk_test, each check within a second:
   - **Each format, each with its own values.** Values over 1, negative values, and values that only the format's
     own precision keeps. The test draws them and resolves into the matching texture. It reads them back on the CPU,
     compares them with the format's encoding, and logs each word.
   - **Clears** of each format, read back the same way.
   - **Blending** (an additive pass) and 2 and 4 samples, for each format the console allows, as found in step 1.
   - **The HDR round trip.** Values up to 8.0 are drawn into a 7e3 target and resolved into a half-float texture.
     That texture is then sampled at 1/8 into a `k_8_8_8_8` target. The result is checked against the values, and
     shown on screen beside the other formats' results.

## Done when

- docs/HARDWARE.md records step 1's answers from the console.
- **On the console,** every format draws, clears and resolves its values, within each format's precision. Blending and
  samples behave as recorded, and the round trip shows on screen.
- **Broken on purpose,** each break fails its check:
  - RB_COLOR_INFO's format left at 0: the format checks fail;
  - a 64-bit target's tiles not doubled: the 64-bit checks fail, or another target is overwritten;
  - the two halves of a 64-bit clear swapped: the 64-bit clear checks fail.
- **xsdk_test, vulkan4xenon_test and vkbench** pass on the console, vkbench's times as before.

## Not in it

- **Multiple render targets:** milestone 43, which also needs xost to write `oC1`-`oC3` (SCOPE.md has them unproven).
- **Targets bigger than eDRAM.** A 64-bit target with its depth fits only up to 1280x672 at 1 sample. Predicated tiling
  is milestone 44.
- **Float depth** (`D24FS8`), and exponent biases on targets and resolves.
- **The `_AS_` formats** (`k_2_10_10_10_AS_10_10_10_10`, `k_2_10_10_10_FLOAT_AS_16_16_16_16`): what sets them apart
  isn't public beyond a line in Xenia.
- **vulkan4xenon's float attachments,** which can follow in its own repository.
