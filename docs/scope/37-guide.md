# Milestone 37: the Guide

Agreed with the user on 2026-10-15, after the user found the Guide never opened in any title built on xsdk: the button
sounded, and the Guide showed only once the title had exited.

## Why

While a title holds the GPU, XAM draws the Guide only through the title's own ring, and the title shows what it drew
(docs/HARDWARE.md, "The Guide shows through VdSwap"):

- **The swap.** Each frame a D3D title calls `VdGetSystemCommandBuffer`, then `VdSwap` into 64 dwords of its ring.
  While the Guide is open, `VdSwap` puts the title's front buffer in texture fetch constant 15 and calls XAM's commands,
  which draw the frame and the Guide over it into a surface of XAM's, 852x480.
- **The surface.** `VdSwap` then hands that surface back through the pointers it was given for the front buffer's
  address, format, colour space and size, and the title shows it, scaled to the output, until the Guide closes.
- **xsdk did neither.** It never called `VdSwap`, and its vblank callback only ever shows its own buffers.

## What it takes

- **`xsdk_present_end` swaps.** Before each frame's fence, `VdGetSystemCommandBuffer`, then `VdSwap` into 64 dwords of
  the ring itself, not the stream, so XAM's commands run from the ring as under D3D. `xsdk_gpu_open` gives the kernel
  the address the swap's fence writes, which XAM reads to know the GPU is done with its commands, and
  `xsdk_gpu_close` takes it back.
- **The flip shows XAM's surface.** When `VdSwap` hands it back, the frame's flip shows it, scaled to the output by a
  plan the display code makes as it makes the 720p mode's, and the next frame of the title's own brings its buffer and
  scaling back. On the kernel's display mode, where xsdk doesn't scale, the Guide still doesn't show; the log says so.
- **After XAM's commands.** They change GPU state and draw in eDRAM. xsdk queues its set-up again, straight into the
  ring as at open and without the ME_INIT, needs the target set again, and drops hierarchical depth. `XsdkGpuStats`
  counts XAM's frames, and vulkan4xenon forgets what it keeps in eDRAM when the count moves.
- **XAM told the title's display.** `xsdk_present_open` sends what D3D sends through
  `VdCallGraphicsNotificationRoutines` event 1: the front buffer's size, the output's, and the scaler between them.
  Without it XAM drew the frame under the Guide from the 640x480 it held from before, the top left of 1280x720
  stretched over the screen (Dusklight on the console, 2026-10-15; agreed then).
- **Left out** until something needs it: a way for a title to pause while the Guide is open (`XN_SYS_UI`).

## Done when

- **xsdk_test** passes on the console, with pacing checks that every frame went through the swap and its fence landed,
  and that no frame showed XAM's surface with the Guide closed; the first fails with the swap broken on purpose.
- **`guide_check`**, the moving bar until the user presses Back, passes on the console while the user opens and closes
  the Guide twice: XAM drew and its surface showed, the title's own frames came back after, and every frame held its
  bar. The user sees the Guide over the whole frame, the bar moving under it. Broken on purpose so XAM's surface never
  shows, the Guide stays hidden and the check fails; with no event 1, the frame under the Guide is its top left.
- **vulkan4xenon_test and vkbench** pass, vkbench's timings within its usual spread.
- **Dusklight** shows the Guide in game, and plays on after it closes.
- HARDWARE.md records the probes, the disassembly, and the runs.

Done on 2026-10-15 (docs/HARDWARE.md, "The Guide, milestone 37"). A title that ends by itself while the Guide is open
hangs as it ends (TODO.md).
