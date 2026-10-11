# Later

Items left over once SCOPE.md's milestones and item 10 were done. None is scoped or started; each needs agreeing
before work begins.

- **Symbol names in `std::stacktrace`.** A trace gives addresses only, as a XEX carries no symbols. `elf2xex` would
  write a symbol map beside the `.xex`, and the runtime would name frames from it (SCOPE.md section 2.5).
- **720p on interlaced outputs (1080i, 480i).** `XSDK_DISPLAY_720P` keeps the kernel's 640x480 there, as scaling on an
  interlaced output isn't tried. Likely pieces: libxenon writes `AVIVO_D1MODE_DATA_FORMAT` 1 for interlaced modes
  (libxenon:drivers/xenos/xenos.c:391), and xenon-emu names a second vertical start phase, 0x65DC,
  `D1SCL_VERT_FILTER_INIT_BOT` (XenosRegisters.h:935).
- **A sharper scaler filter at 1080p.** 1280x720 is scaled with the built-in 2-tap filter, the only setting that came
  out whole. Every variant on the kernel's coefficient table scrambled the image, and 2 taps on the table wasn't tried.
  More taps would mean loading our own coefficients through 0x6578 and 0x657C, in the layout AMD's
  `program_multi_taps_filter` uses (docs/HARDWARE.md, "What AMD's code says about the scaler" and "The built-in 2-tap
  filter scales 1280x720 to 1920x1080 whole").
