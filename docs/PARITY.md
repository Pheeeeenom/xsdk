# What xsdk can do, and what it can't yet

xsdk's goal is to let anyone make homebrew and port games to the Xbox 360 without Microsoft's SDK. This page is the
map: for each area, what a title gets from xsdk today, how to use it, which test proves it on the console, and what's
missing. It was drawn up on 2026-10-15, at xsdk `3e889a7`. SCOPE.md has the milestones and TODO.md the open items;
where they and this page disagree, they win, and this page is out of date.

**Status:**
- **Yes:** works, and a test proves it on the console.
- **Partial:** some of it works, or only through raw kernel calls.
- **No:** not there yet.
- **Out:** won't be in xsdk (end of the page).

**What retail titles call** is judged from Xenia (xenia-project/xenia `95a5c3e`, in `xenos-toolchain/reference/xenia`),
which implements what the games it runs need:
- **The kernel:** its table lists 922 xboxkrnl exports. 239 have code in Xenia, 200 of them more than a stub. xsdk
  imports 55 (`imports/imports.txt`).
- **XAM:** its table lists 1,735 exports. 169 have code in Xenia, 106 of them more than a stub. xsdk imports 27, 22 of
  them for sockets.

## Building and packaging

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Toolchain: GCC 15, binutils, newlib for `powerpc-xsdk-elf` | Yes | `tools/toolchain/build.sh`, in WSL | every build | A prebuilt toolchain |
| A `.xex` the retail loader takes, imports by ordinal | Yes | `tools/elf2xex.py`, `src/xsdk.ld` | `tests/xex_checks.py` | Compression; a title name and icon; signing, so it runs on RGH and JTAG consoles only |
| Make and CMake builds | Yes | `Makefile`; `cmake/xsdk.cmake` and its `xsdk_add_xex()` | `make cmake_check` (`tests/cmake/`) | Shared libraries: static only |
| The boot logo before the title's first frame | Yes | `xsdk_splash_show` (`splash.h`) | xsdk_test | |

## C and C++ runtime

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| C17 on newlib: stdio, strings, math, `malloc` | Yes | newlib | xsdk_test | |
| C++23: exceptions, RTTI, threads, atomics, chrono, filesystem, format | Yes | libstdc++ | `tests/test_cpp_*.cpp` | Names in `std::stacktrace`, which gives addresses only; native TLS, as `thread_local` is emulated (TODO.md) |
| `memcpy` and `memmove` on VMX | Yes | `src/copy.c` | `tests/test_copy.c` | |
| Loop unrolling | Partial | GCC's -O2 default; `-funroll-loops` per file | `tests/probe_unroll.c` | Not on for everything: it made Dusklight bigger and no faster (docs/HARDWARE.md, "Loop unrolling, milestone 35") |

## Memory

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| A heap of all the title's free memory | Yes | `sbrk` (`src/sbrk.c`) | `tests/test_memory.c` | |
| GPU memory, write-combined and physical | Yes | `xsdk_gpu_alloc` (`gpu.h`) | xsdk_test | Small allocations: each takes a 64 KiB page, and only shaders share (TODO.md) |
| Freeing and protecting pages: `munmap`, `mprotect`, queries | No | | | `NtFreeVirtualMemory`, `NtProtectVirtualMemory`, `NtQueryVirtualMemory`, `MmSetAddressProtect` (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc) |
| Memory statistics | Partial | `MmQueryStatistics` (`kernel.h`), raw | `tests/test_memory.c` | An xsdk call |

## Threads and synchronisation

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Threads, joins, detached threads, stack sizes | Yes | pthreads (`src/pthread.c`), `std::thread` | `tests/test_pthread.c`, `test_threads.c` | |
| Placing threads on the 6 hardware threads | Yes | `xsdk_thread_create_on`, `xsdk_thread_move` (`thread.h`) | `tests/test_placement.c` | An unplaced thread runs on its creator's hardware thread |
| Mutexes, condition variables, read-write locks, once, keys | Yes | pthreads | `tests/test_pthread.c` | A mutex holds a pointer to a critical section `malloc`ed on first lock: locking one after its memory is freed corrupts the heap (TODO.md) |
| Priorities | No | `pthread_setschedparam` returns `ENOTSUP` | `tests/test_pthread.c` checks that | `KeSetBasePriorityThread` (xboxkrnl_threading.cc) |
| Semaphores | Partial | `NtCreateSemaphore`, `NtReleaseSemaphore` (`kernel.h`), raw | | POSIX `sem_*` |
| Waiting on several objects; resetting and pulsing events | No | | | `NtWaitForMultipleObjectsEx`, `NtClearEvent`, `NtPulseEvent` (xboxkrnl_threading.cc) |
| Owned mutexes, waitable timers, APCs, lock-free lists, I/O completion ports | No | | | What Windows-style code expects: xboxkrnl_threading.cc, xboxkrnl_io.cc |

## Time

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Wall clock, monotonic clocks, sleeps | Yes | `clock_gettime`, `gettimeofday`, `nanosleep` (`src/time.c`), `std::chrono` | `tests/test_clocks.c`, `test_cpp_chrono.cpp` | Short sleeps: a 1 ms sleep took about 2 ms (docs/HARDWARE.md, "Sleeps, write speeds, and a set-up that wouldn't run from a cached stream") |

## Files, drives and storage

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Files and folders beside the `.xex` (`game:`) | Yes | POSIX `open`, `read`, `stat`, `opendir`, `mkdir`, `rename`, `unlink`; `std::filesystem` | `tests/test_files.c`, `test_cpp_filesystem.cpp` | Asynchronous I/O; files open sharing nothing, so `remove_all` fails on nested folders (TODO.md) |
| Other drives: the hard disk, USB, memory units | Partial | a path starting with `\` goes to the kernel as written (`src/path.c`) | untested | Drive names: no `hdd:`, `usb:` or `dvd:`, as `ObCreateSymbolicLink` (xboxkrnl_ob.cc) isn't imported |
| Free space; flushing to disk | No | | | `NtQueryVolumeInformationFile` (xboxkrnl_io_info.cc), `NtFlushBuffersFile` (xboxkrnl_io.cc) |
| The DVD drive: discs and the tray | No | | | Mounting the drive; `HalOpenCloseODDTray`, which Xenia doesn't implement |

## Save data, content and profiles

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| XAM's asynchronous calls, which the rows below take | No | | | `XOVERLAPPED` and `XamGetOverlappedResult` (xenia:src/xenia/kernel/xam/xam_msg.cc): the first piece everything in this section and the next needs |
| Save games in content packages, on the HDD or a memory unit | No | | | `XamContentCreateEx`, `XamContentClose`, `XamContentDelete`, enumeration (xam_content.cc) |
| Choosing a storage device; its state and free space | No | | | `XamShowDeviceSelectorUI` (xam_ui.cc), `XamContentGetDeviceState` and `XamContentGetDeviceData` (xam_content_device.cc) |
| Sign-in state, profile ID, gamertag | No | | | `XamUserGetSigninState`, `XamUserGetXUID`, `XamUserGetName` (xam_user.cc) |
| Profile settings: invert Y, difficulty, a title's own data | No | | | `XamUserReadProfileSettings`, `XamUserWriteProfileSettings` (xam_user.cc) |
| Achievements | No | | | Through `XMsgStartIORequest` (xam_msg.cc); low priority |

## System UI and the Guide

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| The Guide over the title | Yes | automatic, through `xsdk_present_*` | `tests/guide_check.c`, by hand | The Guide on the kernel's display mode and interlaced outputs (TODO.md) |
| Knowing when to pause for the Guide | Partial | `XamNotifyCreateListener` and `XNotifyGetNext` (`kernel.h`), raw | `tests/guide_check.c` | An xsdk call; `XamIsUIActive` (xam_ui.cc) |
| Ending the title by itself while the Guide is open | No | | | It hangs as it ends; leaving through Xbox Home is fine (TODO.md) |
| On-screen keyboard, message boxes, sign-in prompt | No | | | `XamShowKeyboardUI`, `XamShowMessageBoxUI` (xam_ui.cc), `XamShowSigninUI` (xam_user.cc) |
| Keeping the screen from dimming in cutscenes | No | | | `XamResetInactivity` (xam_input.cc) |
| Where notifications pop up | No | | | `XNotifyPositionUI` (xam_notify.cc) |

## Input

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Controllers: buttons, sticks, triggers, rumble | Partial | `XamInputGetState` and `XamInputSetState` (`kernel.h`), raw; SDL3's gamepads | `tests/test_input.c`; SDL-xenon's `test/xsdk/sdl_check.c` | An xsdk call |
| Controller types: wheel, guitar, arcade stick | No | | | `XamInputGetCapabilities` (xam_input.cc) |
| Chatpad and USB keyboard | No | | | `XamInputGetKeystrokeEx` (xam_input.cc) |
| Headset, microphone, camera, Kinect | No | | | Xenia has stubs only (xam_voice.cc) |

## Audio

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Playing PCM: 6 channels of float at 48 kHz | Partial | the kernel's render driver (`XAudioRegisterRenderDriverClient`, `kernel.h`), raw; SDL3's audio | `tests/test_audio.c`; `tests/audio_check.c`, by ear | An xsdk call; a mixer; recording |
| XMA decoding in hardware | No | | | `XMACreateContext` and the rest of xboxkrnl_audio_xma.cc |
| Speaker configuration; the dashboard's volumes | No | | | `XAudioGetSpeakerConfig`, `XAudioGetVoiceCategoryVolume` (xboxkrnl_audio.cc) |

## Video output

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| 1280x720, scaled to the output up to 1080p | Yes | `xsdk_display_open`, `xsdk_present_*` (`display.h`, `present.h`) | `tests/test_present.c`, `test_scale.c` | |
| 2 or 3 front buffers, paced to vblank, swap intervals | Yes | `xsdk_present_open` | `tests/test_pacing.c` | |
| Other modes: 1080p, 480p and the rest | No | `XSDK_DISPLAY_KERNEL` keeps the kernel's own | | |
| 720p on interlaced outputs, 480i and 1080i | No | the kernel's 640x480 there | | TODO.md |

## GPU

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Draws, indexed or not: points, lines, line strips, triangle lists, strips and fans; up to 16 vertex streams (`XSDK_VERTEX_STREAMS`); 18 vertex formats, their components picked in any order | Yes | `gpu.h`, `shader.h` | `tests/test_triangle.c`, `test_indexed.c`, `test_vertex.c` | More than 65,535 indices a draw; primitive restart (TODO.md); 10-bit vertex formats |
| Shader constants, set by the CPU or loaded by the GPU from memory | Yes | `gpu.h` | `tests/test_constants.c` | |
| Depth, stencil, blending, write masks, culling, depth bias, scissor, viewport | Yes | `gpu.h` | `tests/test_depth.c`, `test_raster.c` | A console run of two-sided stencil, built for vulkan4xenon milestone 26; a constant blend colour; wireframe and point fill (TODO.md) |
| Hierarchical depth | Yes | automatic after a depth clear, at 1 sample | `tests/test_hiz.c` | Multisampled targets |
| Textures: 24 formats with DXT1, 3 and 5, DXN and DXT5A; signed and gamma reads; linear and tiled; mips; anisotropy; swizzles | Yes | `texture.h` | `tests/test_texture.c`, `test_formats.c`, `test_anisotropy.c` | 32-bit floats in two and four channels, signed DXN and DXT5A (TODO.md); 128-bit textures (SCOPE.md); LOD clamps; mipped linear textures (TODO.md) |
| Textures sampled in vertex shaders too, both stages' among the 32 fetch constants | Yes | xost (`xenos-toolchain/docs/xsc.md`, "Vertex textures"); `xsdk_shader_move_textures` (`shader.h`) | `tests/test_texture.c` moves a fetch; vulkan4xenon_test draws through it, on the console (vulkan4xenon milestone 22) | |
| Mirror-once, halfway and border addressing, a black or white border | Yes | `XSDK_ADDRESS_*` and `XsdkSampler`'s `border` (`texture.h`) | `tests/test_sampling.c` | The YCbCr borders (TODO.md) |
| Cube textures with mips, and 3D textures of one level, tiled | Yes | `xsdk_texture_init_cube`, `xsdk_texture_init_3d` and the slice uploads (`texture.h`) | `tests/test_cube_volume.c` | Linear ones; a 3D texture's mips; resolving into a face or slice (TODO.md) |
| Render targets in eDRAM, 1, 2 or 4 samples | Yes | `xsdk_gpu_set_target*` | `tests/test_render.c`, `test_multisample.c` | Formats other than 32-bit RGBA, and several targets, multisampled; targets bigger than eDRAM (predicated tiling) (TODO.md, SCOPE.md) |
| Float, 10-bit and 64-bit targets: 2_10_10_10, 7e3, 16_16_16_16, 16- and 32-bit floats, at 1 sample | Yes | `xsdk_gpu_set_targets`, `xsdk_gpu_resolve_target` | `tests/test_targets.c` | 16_16, 32_32_FLOAT and gamma targets (TODO.md); blending into the 16- and 32-bit floats, which Xenos doesn't (docs/HARDWARE.md) |
| Four targets at once (MRT), each blended and masked on its own, at 1 sample | Yes | `xsdk_gpu_set_targets`, `xsdk_gpu_set_blends`; xost's outputs at locations 1 to 3 | `tests/test_targets.c`; xost's `tests/test_color_outputs.py` | Several targets multisampled (TODO.md) |
| Resolves of colour and depth, to memory and textures, with a clear | Yes | `xsdk_gpu_resolve*` | `tests/test_render.c`, `test_depth.c` | Part of a target (TODO.md); clearing eDRAM without a copy (SCOPE.md) |
| Command buffers recorded once and run many times | Yes | `xsdk_gpu_record_*`, `xsdk_gpu_execute` | `tests/test_replay.c` | Fences and resolves inside a recording; recording on several threads, as the ring takes one at a time |
| Occlusion queries and GPU timers | No | | | Queries (TODO.md); GPU timing exists only in a probe (`tests/probe_gpu_draw_cost.c`) |
| Memory export from shaders | No | | | xost doesn't emit it (SCOPE.md) |

## Shaders

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| GLSL to Xenos microcode, offline | Partial | glslang to SPIR-V, then xost (`xenos-toolchain/`) | xost's tests | A subset of GLSL (`xenos-toolchain/docs/xsc.md`): no bitwise operations; `if` on data as selects of both arms, a vertex shader's and one inside a loop too, and on a uniform in execs on the predicate; loops unrolled, or hardware loops where an int in a block counts them, with a break ending a pass on a uniform; no `switch` or `continue` |
| SPIR-V to microcode on the console | Yes | `xsdk_xsc_compile` (`xsc.h`) | `tests/test_xsc.c` | Compiling GLSL on the console (docs/scope/shader-compiling.md) |
| HLSL | No | | | |

## Networking

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| TCP and UDP sockets, `poll`, `select`, non-blocking | Yes | BSD sockets (`src/socket.c`) | `tests/test_net.c`, with `tools/net_echo.py` on the PC | IPv6; why a connect failed, as XAM refuses `SO_ERROR` (src/socket.c) |
| DNS | Yes | `getaddrinfo` (`src/netdb.c`) | `tests/test_net.c` | |
| TLS 1.2 | Yes | the mbedtls-xenon fork | docs/HARDWARE.md, "TLS 1.2 through Mbed TLS" | TLS 1.3; an HTTP client |
| System Link | No | | | XNet's secure transport (xam_net.cc) |

## Libraries ported to xsdk

Each in its own repository under xenosworks.

| Library | Status | What it gives | Verified by | Missing |
|---|---|---|---|---|
| SDL3 (SDL-xenon) | Partial | audio, up to four controllers, paths, threads, timers | `test/xsdk/sdl_check.c` there | A video, render or GPU driver: titles draw with xsdk's GPU layer or vulkan4xenon (SDL-xenon's docs/README-xsdk.md) |
| Vulkan 1.0, a subset (vulkan4xenon) | Partial | a Vulkan layer on xsdk's GPU layer | its test title and vkbench | What its docs/SUPPORT.md leaves out |
| Mbed TLS 3.6 (mbedtls-xenon) | Yes | TLS 1.2 and its cryptography | its self-tests and HTTPS check | TLS 1.3 |

## System information and control

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Ending the title, back to the dashboard | Yes | `exit` (`src/exit.c`) | every title | |
| Random numbers | Yes | `getentropy` (`src/entropy.c`) | mbedtls-xenon's checks | |
| Language, region, time zone, AV settings | No | | | `XGetLanguage` (xam_info.cc), `ExGetXConfigSetting` (xboxkrnl_xconfig.cc) |
| Launching another `.xex`, and passing it data | No | | | `XamLoaderLaunchTitle`, `XamLoaderSetLaunchData` (xam_info.cc) |
| Rebooting and powering off | No | | | `HalReturnToFirmware` (xboxkrnl_hal.cc) |
| The SMC: LEDs, temperatures, fan, power button, AV cable | No | | | `HalSendSMCMessage` and `HalGetCurrentAVPack`, which Xenia doesn't implement, so only the console can settle them |
| Hashes and encryption for a title's own data | No | | | `XeCryptSha`, `XeCryptAesCbc` (xboxkrnl_crypt.cc), `RtlComputeCrc32` (xboxkrnl_rtl.cc) |

## Debugging and profiling

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| A log file per title, readable while it runs; `stdout` and `stderr` into it | Yes | `xsdk_log_*` (`log.h`) | `tests/test_files.c` | The log over the network |
| `DbgPrint` | Yes | `kernel.h` | | |
| Crash reports with function names | No | | | |
| Counters: the ring, presents, thread waits | Yes | `xsdk_gpu_stats`, `xsdk_present_stats`, `xsdk_thread_wait_counts` | xsdk_test | A CPU profiler; a GPU capture tool |

## Documentation and samples

| Capability | Status | Where | Missing |
|---|---|---|---|
| How the SDK was built, and every fact the console settled | Yes | SCOPE.md, docs/HARDWARE.md | It's a lab notebook, not a guide |
| Getting started | Partial | README.md's Building and Testing, for working on xsdk itself | Toolchain to a title running on the console, for someone new |
| An API guide: call order, lifetimes, what undoes what | Partial | the headers' one-line comments | |
| Pitfalls: big-endian data, stores to GPU memory, the Xenia differences | Partial | spread through docs/HARDWARE.md and CLAUDE.md | One page |
| Samples to start from | No | `tests/` only | |

## Out of xsdk

- **Xbox Live and anything online through Microsoft:** signing in to Live, matchmaking, presence, friends, parties, the
  marketplace, Live's secure transport. xsdk stays clear of anything that could serve online modding.
- **The hypervisor:** no HV or HVX calls, and nothing that reaches the console's keys (CODE_GUIDELINES.md 1.2).

## The order proposed for the gaps

What a porter would hit first, each a milestone agreed when it starts:

1. **Catching faults:** a report of every CPU exception in the log, with its address, registers and stack, and the
   title ended back at the dashboard (SCOPE.md, milestone 40).
2. **Docs and samples,** which make everything after them easier to use, for people and for the AI assistants most of
   them will work through (SCOPE.md, milestone 41).
3. **The log over the network,** so a title's failure can be seen without copying files back.
4. **XAM's asynchronous calls, then save data and drives:** content packages, the device selector, `hdd:`, `usb:` and
   `dvd:`.
5. **Profiles and system UI:** sign-in, the keyboard, message boxes, pausing for the Guide, and ending under it.
6. **Threading for Windows-style code:** waits on several objects, event resets, priorities, timers, semaphores.
7. **Drawing for SDL3 ports:** a render driver, as SDL3 has none on the console.
8. **The GPU's missing features:** float and 64-bit targets, MRT, predicated tiling, cube and 3D textures, queries.
9. **Audio:** XMA decoding, speaker configuration.
10. **Input and the system:** keyboards, controller types, language and region, launching titles, the SMC.
