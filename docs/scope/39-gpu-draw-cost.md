# Milestone 39: the GPU's cost of a draw

Agreed with the user on 2026-10-15, after milestone 38, as the next step in narrowing what drawing through xsdk costs.

## Why

After milestone 38 the CPU writes vkbench's cube in 0.29 us, and the GPU is the slower of the two (docs/HARDWARE.md,
"The cost of a draw, milestone 38"):
- **The time to the fence didn't move.** From the frame's start to its fence, `draws_1000` takes 994 us, as it did
  before the CPU got twice as fast.
- **About 0.38 us a cube on the GPU.** That's `draws_1000`'s time less `draws_250`'s, over the 750 cubes between.
- **State changes cost the GPU most.** `states_1000` takes 1,691 us to its fence, against the CPU's 463: a texture
  change every draw, and a shader change every second.

What each draw puts in the GPU's way, none of it timed on the GPU yet:
- **Constants:** two groups of 4 constants, 32 dwords, with up to 6 dwords of padding.
- **Two register writes before every draw:** `COHER_DEST_BASE_1` and `VGT_INDX_OFFSET`, as libxenon writes them
  (libxenon:drivers/xenos/xe.c:191-200).
- **The indexed draw itself:** `DRAW_INDX`, its indices fetched from memory.
- **On a texture or vertex change:** a fetch constant group and a fetch constant flush.
- **On a shader change:** an `IM_LOAD` of both shaders.

## What it takes

1. **A probe**, `tests/probe_gpu_draw_cost.c`, times only the GPU. It records a batch of vkbench's cubes into a command
   buffer once, then times the GPU from kick to fence. It does this at two batch sizes, so fixed costs cancel and the
   difference is the draws'.
   - **The shaders** are vkbench's pair: a matrix from push constants placing the cube, and the texture times a tint.
     They come through xost's glslang and compiler at xsdk's register split (`tools/gen_draw_cost_shaders.py`).
   - **The cubes** are a few pixels each and spread over the screen, so pixel work stays small.
   - **The variants:**
     - the draw as the layer writes it;
     - the draw alone, and with each constant group alone;
     - the draw packet without its two register writes;
     - constants loaded from memory instead;
     - a texture change every draw;
     - a shader change every draw.
2. **The cuts**, agreed with the user on 2026-10-15 from the probe's first runs (docs/HARDWARE.md, "The GPU's cost of a
   draw"):
   - **Load only the shader that changed.** `xsdk_gpu_set_shaders` loads a shader only if it isn't the one loaded, and
     writes nothing when neither changed. Setting the same program again cost the GPU 0.11 us a draw; reloading only
     the pixel shader saved 0.07 us a change. What's loaded is forgotten wherever anything else loads microcode or
     runs packets the layer didn't track: the set-up, a resolve, XAM's commands, a recorded buffer, and a shader freed
     or patched.
   - **The draw's two register writes once, with the target.** `COHER_DEST_BASE_1` and `VGT_INDX_OFFSET` are written
     0 before every draw. They're written with the target instead, as every draw needs a target set after anything
     that could change them.
   - **The fetch constant flush after a texture change,** probed first: left out if the textures still come out right
     without it, else kept and the record says so.

## Done when

- HARDWARE.md records the probe's numbers before and after, from the console.
- **The probe:** a cube with its program set again costs what a cube does, and a pixel shader change costs what the
  pixel-only reload did.
- **vkbench:** `states_1000`'s time from the frame's start to its fence falls by 4% or more, from 1,691 us. The draw
  scenes' times hold or fall, both backends draw the same pictures, and all 14 checks pass.
- **xsdk_test:** it passes on the console. Broken on purpose, so `xsdk_gpu_set_shaders` looks only at the vertex
  shader and a new pixel shader isn't loaded, its tests that draw with more than one program fail. (Not across a
  resolve: draws would run the resolve's microcode, which could hang the GPU rather than fail a check.)
- **vulkan4xenon_test** passes on the console.

Done on 2026-10-15 (docs/HARDWARE.md, "The GPU's cost of a draw, milestone 39"). `states_1000` came 3.6% closer to its
fence, not 4%; the user accepted that as within noise of what the target was set from. The cuts showed the GPU's
0.2 us a draw had been the two register writes before each draw, and that constant writes between draws hold the GPU
the same way, which is left in TODO.md.
