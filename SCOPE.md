# Xbox 360 SDK: scope of the SDK layer and the Xenos GPU layer

**Goal.** A from-scratch Xbox 360 homebrew SDK that builds `.xex` executables that the retail kernel and dashboard
launch, on a modded console (RGH/JTAG). On top of it comes a GPU layer that drives Xenos directly. A Vulkan
translation layer comes later and is out of this scope. So are games.

**Constraints.**
- No Microsoft tooling and no leaked SDK material.
- No OpenXeChain.
- Public reverse-engineering, mainly the Xenia emulator's source, is the reference, and every fact below cites it.

**Sources.**
- **Xenia.** Xenia's own copy is in `xenos-toolchain.zip` under `xenos-toolchain/reference/xenia` (commit `95a5c3e`).
  It's cited below as `xenia:<path>:<line>`, paths relative to `reference/xenia/`.
- **libxenon.** Its copy is in the same zip, cited as `libxenon:<path>:<line>`, relative to
  `reference/libxenon/libxenon/`.
- **The user's toolchain** is cited as `xost:<path>:<line>`, relative to `xenos-toolchain/`. It's been part of xsdk since
  2026-10-14; its commits before then are cited by the hashes of its own repository, xenosworks/xenos-toolchain.
- **Unverified.** Where something isn't settled by these sources, it's marked unverified.

## 1. What already exists: the user's shader toolchain ("xost")

- **What it is:** an open shader toolchain for Xenos with no Microsoft tooling, verified on a real RGH console through
  libxenon (xost:README.md:6-8, 32-41).
- **Its parts** (xost:README.md:48-78, docs/HANDOFF.md:9-24):
  - `xdis`, a microcode disassembler. Its ISA tables are transcribed from Xenia's `ucode.h` under BSD-3.
  - `xasm`, encoders, a program builder, and a 16-register allocator.
  - `xsc`, a GLSL to SPIR-V to Xenos compiler. It handles loops, branch-free if/else and about 35 builtins
    (xost:docs/xsc.md:17-74).
  - `xcomb`, a C99 combiner generator that runs on the console.
- **Its output** is libxenon's shader container, which `Xe_LoadShaderFromMemory` loads (xost:docs/container.md:1-37).
  The container's shader record carries the values for `SQ_PROGRAM_CNTL` and `SQ_CONTEXT_MISC`
  (xost:docs/container.md:34-35). Vertex shaders contain template vertex fetches that libxenon patches at load time
  to the live vertex format (xost:docs/container.md:48-55).
- **Still open:**
  - Some ISA features are unproven on hardware: boolean constants, scalar co-issue, predication, ubyte4 vertex formats,
    depth export and multiple render targets (xost:README.md:89-91).
  - The licence isn't chosen yet (xost:docs/license-audit.md:59-87).
  - The test corpus is XDK compiler output of disputed provenance; the toolchain only reads it, as test vectors
    (xost:docs/license-audit.md:89-133).
- **What changes for a dashboard `.xex`:** nothing in the shader path; Xenos runs the same microcode however the
  program was launched. What libxenon did around it must now be done by the SDK's own GPU layer (section 3):
  - loading the container;
  - patching the vertex fetches;
  - setting the program-control registers.

## 2. The SDK layer

### 2.1 Toolchain

- **The compiler.** The user has a modern GCC for libxenon. libxenon's own build script targets `xenon` with GCC 9.2,
  binutils with VMX128 patches, and newlib, configured `--with-cpu=cell`
  (xost:reference/libxenon/toolchain/build-xenon-toolchain:6, 11, 126-183). It produces ELF.
- **The executable it must feed.** The kernel loads a PE image inside the `.xex`. Xenia's loader checks for:
  - a DOS header (`MZ`) and an NT header (`PE\0\0`);
  - machine `0x01F2` (PowerPC big-endian) with `IMAGE_FILE_32BIT_MACHINE`;
  - a 32-bit optional header of standard size;
  - subsystem `IMAGE_SUBSYSTEM_XBOX` (14).

  Sources: xenia:src/xenia/cpu/xex_module.cc:776-815, xenia:third_party/pe/pe_image.h:233.
- **Recommendation.** Keep GCC and ELF, and write the SDK's own linker post-step (`elf2xex`). It lays the ELF's
  segments out as a PE image (headers, sections, base relocations if needed), then wraps that image in XEX2
  (section 2.2). That's simpler than teaching binutils a PE target for this machine type.
- **Unverified, ask the user:** the code model and ABI the kernel expects:
  - 32-bit addresses with 64-bit registers;
  - which register holds the processor control region pointer;
  - stack alignment and the calling convention used across kernel calls.

  Xenia's PowerPC frontend and kernel shims define these, and should be read before writing `crt0`.

### 2.2 The executable: XEX2

- **The header** (xenia:src/xenia/kernel/util/xex2_info.h:548-556):
  - magic `XEX2`;
  - module flags, header size, security-info offset;
  - a count of optional headers, each a key and a value or an offset (`xex2_opt_header`, xex2_info.h:539).
- **Optional headers a minimal title needs** (keys at xex2_info.h:279-308):
  - `ENTRY_POINT` (0x00010100);
  - `IMAGE_BASE_ADDRESS` (0x00010201);
  - `IMPORT_LIBRARIES` (0x000103FF);
  - `TLS_INFO` (0x00020104);
  - `DEFAULT_STACK_SIZE` (0x00020200);
  - `DEFAULT_HEAP_SIZE` (0x00020401);
  - `SYSTEM_FLAGS` (0x00030000);
  - `EXECUTION_INFO` (0x00040006): title ID, version, media;
  - `FILE_FORMAT_INFO` (0x000003FF).
- **System flags** are listed at xex2_info.h:124-153. A homebrew title may want `NO_FORCED_REBOOT` and
  `HANDLE_GAMEPAD_DISCONNECT`; which ones matter is unverified.
- **File format:**
  - encryption is none or normal, and compression is none, basic, normal (LZX) or delta
    (xex2_info.h:312-320, 364-372);
  - the security info (xex2_info.h:570-588) holds an RSA signature, section and header digests, the image size, the
    load address, and page descriptors that give each page range's type and digest (xex2_info.h:559-568).
- **Recommendation:** emit uncompressed (`XEX_COMPRESSION_NONE`), unencrypted (`XEX_ENCRYPTION_NONE`) basefiles, with
  correct digests and an empty signature.
- **Unverified, test first:** which checks a patched RGH/JTAG kernel still enforces (signature, header digest, page
  digests, media and region flags). Milestone 1 answers it on the user's console. Computing every digest correctly
  costs little and removes the guesswork.

### 2.3 Imports by ordinal

- **How the loader resolves imports.** The import-libraries header holds a string table and one record per library
  (xenia:src/xenia/kernel/util/xex2_info.h:502-519): ID, version, minimum version, name index, count, and the address
  of each import slot. Each slot's word encodes the import's type in its top byte and the ordinal in its low 16 bits
  (xenia:src/xenia/cpu/xex_module.cc:1123-1134):
  - **Type 0, a variable:** a word the loader overwrites with the export's address, for kernel variables such as
    `VdGlobalDevice` (xex_module.cc:1154-1197).
  - **Type 1, a function thunk:** 16 bytes of code. On load it reads `li r3, 0` / `li r4, ordinal` /
    `mtspr CTR, r11` / `bctr`, and the console rewrites it to load r11 with the export's address
    (xex_module.cc:1218-1240).
- **The SDK provides:**
  - a header of ordinals;
  - an assembler macro that emits the variable slot and the 16-byte thunk for each import;
  - the per-library records, emitted by `elf2xex`.
- **Minimal first set,** with ordinals from Xenia's export tables (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc
  and xenia:src/xenia/kernel/xam/xam_table.inc, lines given):

| Area | xboxkrnl.exe | Ordinal (line) |
|---|---|---|
| Debug | `DbgPrint` | 0x003 (17) |
| Exit | `HalReturnToFirmware` | 0x028 (54) |
| Threads | `ExCreateThread`, `ExTerminateThread` | 0x00D (27), 0x019 (39) |
| | `KeDelayExecutionThread` | 0x05A (104) |
| | `KeSetAffinityThread`, `KeSetBasePriorityThread` | 0x097 (165), 0x099 (167) |
| Sync | `NtCreateEvent`, `NtSetEvent` | 0x0D1 (223), 0x0F6 (260) |
| | `NtCreateSemaphore`, `NtReleaseSemaphore` | 0x0D5 (227), 0x0F3 (257) |
| | `KeWaitForSingleObject` | 0x0B0 (190) |
| | `NtWaitForSingleObjectEx`, `NtWaitForMultipleObjectsEx` | 0x0FD (267), 0x0FE (268) |
| | `RtlInitializeCriticalSection`, `RtlEnterCriticalSection`, `RtlLeaveCriticalSection` | 0x12E, 0x125, 0x130 |
| TLS | `KeTlsAlloc`, `KeTlsGetValue`, `KeTlsSetValue` | 0x152, 0x154, 0x155 |
| Memory | `NtAllocateVirtualMemory`, `NtFreeVirtualMemory` | 0x0CC (218), 0x0DC (234) |
| | `MmAllocatePhysicalMemoryEx`, `MmFreePhysicalMemory`, `MmGetPhysicalAddress` | 0x0BA, 0x0BD, 0x0BE |
| Files | `NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile`, `NtClose` | 0x0D2, 0x0DF, 0x0F0, 0x0FF, 0x0CF |
| | `NtQueryInformationFile`, `NtQueryFullAttributesFile`, `NtQueryDirectoryFile` | 0x0E8, 0x0E7, 0x0E4 |
| | `ObCreateSymbolicLink` (device paths to drive letters) | 0x103 (273) |
| Time | `KeQueryPerformanceFrequency`, `KeQuerySystemTime` | 0x083 (145), 0x084 (146) |
| Audio | `XAudioRegisterRenderDriverClient`, `XAudioUnregisterRenderDriverClient` | 0x1F3 (513), 0x1F4 (514) |
| | `XAudioSubmitRenderDriverFrame`, `XAudioGetSpeakerConfig` | 0x1F5 (515), 0x1FF (525) |
| Video | the `Vd*` set | section 3.1 |

| Area | xam.xex | Ordinal (line) |
|---|---|---|
| Input | `XamInputGetCapabilities`, `XamInputGetState`, `XamInputSetState` | 0x190 (197), 0x191 (198), 0x192 (199) |
| Exit | `XamLoaderTerminateTitle` | 0x1A9 (222) |
| Launch | `XamLoaderLaunchTitle` | 0x1A4 (217) |
| UI | `XamShowMessageBoxUI` | 0x2CA (510) |

- **Audio** goes through the kernel's render-driver client: register a callback, then submit PCM frames. Xenia's
  `xboxkrnl_audio.cc` is the reference, with `xboxkrnl_audio_xma.cc` for XMA hardware decoding.
- **Unverified:** the exact PCM frame format and rate. Read those files before milestone 2.

### 2.4 Runtime and startup

- **What the loader provides:**
  - the entry point from `ENTRY_POINT`;
  - the main thread's stack from `DEFAULT_STACK_SIZE`;
  - a heap size from `DEFAULT_HEAP_SIZE`;
  - TLS from `TLS_INFO` (slot count, raw data address and size; xenia:src/xenia/kernel/util/xex2_info.h:419-424).
- **What the SDK supplies:**
  - **`crt0`:** set up TLS for the main thread, run C++ static constructors, call `main`, then exit to the dashboard
    with `XamLoaderTerminateTitle`.
  - **newlib's system calls on the kernel:** `read`/`write`/`open`/`close`/`lseek`/`stat` on `Nt*File`, `sbrk` on
    `NtAllocateVirtualMemory`, `gettimeofday` on `KeQuerySystemTime`, and the time base for timing.
  - **A small threads library:** `ExCreateThread` and the events, semaphores and critical sections above.
- **Recommendation:** C with newlib first, then the C++23 runtime (section 2.5).

### 2.5 The C++23 runtime

**Target: C++23 (`-std=c++23`), with libstdc++.** The language features need only the compiler. The work is in
libstdc++'s dependencies on the platform, which C++20 already needs; the rest of C++23's library is header-only:
`std::expected`, `std::mdspan`, the ranges additions, `std::flat_map` and others. GCC supports most of C++23 from
version 13 on, so the user's GCC version decides the reach (section 5). libxenon's own script stops at GCC 9.2
(xost:reference/libxenon/toolchain/build-xenon-toolchain:11).

What the SDK supplies, piece by piece:
- **Static initialisation.** `elf2xex` keeps `.init_array`/`.fini_array`, and `crt0` runs them through newlib's
  `__libc_init_array` before `main`.
- **Allocation.** newlib's `malloc` with `memalign` and `aligned_alloc` working, so aligned `operator new` serves
  VMX128's 16-byte vectors.
- **Threads, mutexes, condition variables** (`std::thread`, `std::mutex`, `std::condition_variable`, `std::jthread`,
  `std::stop_token`).
  - libstdc++ reaches threads through its gthreads layer. The SDK supplies one built on `ExCreateThread`, critical
    sections and kernel events (section 2.3).
  - Either write a small POSIX threads layer over those and use libstdc++'s POSIX gthreads, or write a gthreads file
    for this target. **Recommendation:** the POSIX layer, since newlib and other libraries can use it too.
- **`thread_local`.** Static TLS doesn't match GCC's PowerPC TLS models.
  - Under the retail kernel, r13 holds the thread's processor control region (xenia:src/xenia/cpu/thread_state.cc:55).
  - The region's `tls_ptr` points to the thread's TLS block, which holds the XEX's TLS data and slots
    (xenia:src/xenia/kernel/xthread.cc:303-321, 361-364). Titles reach their static TLS through it
    (xthread.cc:303-304).
  - GCC instead expects a variable at a fixed offset from the thread pointer register itself.
  - **Recommendation:** start with GCC's emulated TLS (`-femulated-tls`), whose runtime hooks map onto `KeTlsAlloc`,
    `KeTlsGetValue` and `KeTlsSetValue` (0x152, 0x154, 0x155). A native TLS model, a GCC change that loads `tls_ptr`
    from `0(r13)` first, can come later if emulated TLS is too slow.
- **Atomics** (`std::atomic`, including 64-bit). GCC emits the CPU's load-reserved/store-conditional pairs inline,
  `lwarx`/`stwcx.` and `ldarx`/`stdcx.`. libatomic covers anything wider.
- **`std::atomic::wait` and `notify`** (C++20). libstdc++ implements them over a futex on Linux and over its own
  mutex and condition variable elsewhere. The SDK gets the latter through gthreads. That path can spin and yield
  before sleeping, which costs real CPU, so test that it sleeps promptly.
- **Exceptions and RTTI.** RTTI needs only the compiler. Exceptions need libgcc's DWARF unwinder, and that needs:
  - `elf2xex` keeping `.eh_frame` and `.gcc_except_table`;
  - `crt0` registering the frames with `__register_frame_info`, since there's no `dl_iterate_phdr` without an ELF
    loader.

  Whether the kernel's own exception dispatch gets in the way of libgcc's unwinding is unverified. The test title
  checks a throw across threads and across a kernel callback.
- **`std::chrono`.**
  - `system_clock` comes from `KeQuerySystemTime` (0x084), NT time: 100 ns since 1601.
  - `steady_clock` comes from the CPU's time base (`mftb`), scaled by its frequency: 49.875 MHz, the CPU's 3192 MHz
    over 64, measured against the display on the console. `KeQueryPerformanceFrequency` (0x083) reports 50 MHz, 0.25%
    fast (docs/HARDWARE.md, "The time base runs at 49.875 MHz").
- **`std::filesystem`.** libstdc++'s implementation sits on POSIX calls. The SDK maps `stat`, `opendir`/`readdir` and
  `mkdir` onto `NtQueryFullAttributesFile`, `NtQueryDirectoryFile` and `NtCreateFile` (section 2.3), and drive
  letters onto device paths with `ObCreateSymbolicLink`.
- **`std::print` and `std::println`** (C++23) write to stdout. The SDK points stdout and stderr at the log file
  (section 2.6), and at `DbgPrint` if that channel turns out to work.
- **`std::stacktrace`** (C++23) is the one hard piece: libstdc++ builds it on libbacktrace reading DWARF from an ELF on
  disk, and a XEX carries none. **Later item:** have `elf2xex` write a symbol map beside the `.xex` and symbolise
  addresses on the host. Until then, `std::stacktrace` returns addresses only, or is left out.

### 2.6 Deploying and testing

- **To settle with the user before milestone 1:** how a `.xex` gets onto their console and runs. Likely options are a
  dashboard's FTP server and launcher, or a USB drive.
- **Logging:** how logs come back without a debug kit. `DbgPrint` output needs a debug channel that a retail kernel
  may not expose, which is unverified. A log file on USB or HDD always works, and is the recommended first channel.
- **Recommendation:** a host-side script that uploads, launches and fetches the log in one step, through FTP and a
  command port.

## 3. The Xenos GPU layer

### 3.1 How a title drives Xenos under the retail kernel

The kernel doesn't provide a graphics driver. A title's own user-mode library writes PM4 command packets into a ring
buffer and uses the kernel's `Vd*` exports for setup, interrupts and the swap.

1. **Memory.** Allocate the ring buffer and all GPU resources with `MmAllocatePhysicalMemoryEx` (0x0BA). The comment
   at xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:236-238 says the ring is write-combined physical memory, and
   write-combine is a protection bit (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:37-38). Get the GPU address
   with `MmGetPhysicalAddress` (0x0BE).
2. **Engines.** `VdInitializeEngines` (0x1C2) takes a callback, its argument, and pointers to **PFP and ME microcode**
   for the command processor (xboxkrnl_video.cc:193-201; Xenia stubs it). Section 3.2 has the answer: the SDK passes
   no microcode.
3. **Ring buffer.** `VdInitializeRingBuffer` (0x1C3) takes the ring's physical address and log2 of its size
   (xboxkrnl_video.cc:235-241). `VdEnableRingBufferRPtrWriteBack` (0x1B6) takes a memory location for the read
   pointer and a block size, usually log2 6 (xboxkrnl_video.cc:244-249).
4. **Submitting.**
   - GPU registers are memory-mapped at `0x7FC80000` (xenia:src/xenia/gpu/graphics_system.cc:92-95).
   - The title advances the ring by writing `CP_RB_WPTR`, register 0x01C5, at `0x7FC80000 + 0x1C5 * 4`
     (graphics_system.cc:208-213).
   - Progress is read back from the read-pointer write-back location.
5. **Interrupts.** `VdSetGraphicsInterruptCallback` (0x1D5) installs a callback with two arguments
   (xboxkrnl_video.cc:225-233). The command stream raises interrupts with `PM4_INTERRUPT`.
6. **Display.**
   - `VdGetCurrentDisplayInformation` (0x1BA) and `VdQueryVideoMode` (0x1CA) give the mode.
   - `VdSwap` (0x25B) presents (xboxkrnl_video.cc:352-420). Its arguments: a pointer to 64 dwords the caller reserved
     in the ring for it, the front buffer's texture fetch constant as a D3D-style texture header keeps it, a write-back
     pointer, the system command buffer from `VdGetSystemCommandBuffer` (0x1BD), the front buffer's address, format
     (`k_8_8_8_8` or a 2-10-10-10 format), colour space, width and height.
   - Xenia fills the reserved region itself (xboxkrnl_video.cc:405-420). On the user's console the kernel writes only a
     fence there and doesn't flip. The SDK presents by setting the display controller's surface registers and filling
     its LUT instead (docs/HARDWARE.md).
   - Other `Vd*` calls appear at engine setup in titles: `VdSetSystemCommandBufferGpuIdentifierAddress` (0x1D9),
     `VdInitializeScalerCommandBuffer` (0x1C5), `VdRetrainEDRAM` (0x269), `VdShutdownEngines` (0x1DC). Milestone 4
     needed only `VdShutdownEngines` of these: `VdInitializeEngines`, the two ring calls and `VdShutdownEngines` are
     enough to submit packets and fences (docs/HARDWARE.md).

### 3.2 The command processor microcode: a decision for the user

- **What a title passes:** `VdInitializeEngines` takes the command processor's PFP and ME microcode from the title.
  Retail titles carry it in Microsoft's D3D library.
- **What libxenon does:** it embeds its own copy, `xenos_ucode0` (0x120 words) and `xenos_ucode1` (0x900 words), and
  writes it into the CP itself on bare metal (libxenon:drivers/xenos/ucode.c, drivers/xenos/xe.c:224-252, 336-366).
- **The problem:** this is firmware that can't be written from scratch, and libxenon's copy has the same disputed
  provenance as xost's corpus.
- **The options:**
  1. **Call `VdInitializeEngines` with microcode read back from the GPU at run time.** The dashboard has already
     loaded it, and libxenon's verify loop shows the ME RAM can be read back through register 0x07E8
     (xe.c:240-247). Whether a title can read that register under the retail kernel is unverified.
  2. **Skip `VdInitializeEngines`** and use the command processor as the dashboard left it. Whether the kernel lets
     a title do that is unverified.
  3. **Ship libxenon's copy.** This is legally the same question as the corpus.
- **Recommendation:** try option 1 at milestone 4, then option 2, and decide on 3 only if both fail.
- **Answer (milestone 4, docs/HARDWARE.md):** option 1 fails: the PFP microcode doesn't read back, and reading either
  microcode RAM hangs the GPU. Option 2 works in a different form. XAM's own D3D device uses the command processor
  while a title runs, and `VdInitializeEngines` is what makes it give the GPU up. Called with no microcode, it leaves a
  command processor that runs the title's packets, and `VdShutdownEngines` hands the GPU back to XAM. The SDK
  supplies no microcode.

### 3.3 Packets

- **Formats.**
  - Type-0 packets write consecutive registers (`MakePacketType0`, xenia:src/xenia/gpu/xenos.h:1637).
  - Type-3 packets carry an opcode (`MakePacketType3`, xenos.h:1657).
- **The opcodes a minimal driver needs** (xenos.h:1573-1622):
  - `ME_INIT` 0x48, `NOP` 0x10, `INDIRECT_BUFFER` 0x3F;
  - `WAIT_FOR_IDLE` 0x26, `WAIT_REG_MEM` 0x3C, `REG_RMW` 0x21;
  - `EVENT_WRITE` 0x46, `EVENT_WRITE_SHD` 0x58: fences that write to memory;
  - `DRAW_INDX` 0x22, `DRAW_INDX_2` 0x36: indices in memory or in the packet;
  - `SET_CONSTANT` 0x2D, `LOAD_ALU_CONSTANT` 0x2F, `SET_SHADER_CONSTANTS` 0x56;
  - `IM_LOAD` 0x27, `IM_LOAD_IMMEDIATE` 0x2B: shader microcode by pointer or inline;
  - `INTERRUPT` 0x54.
- **Not hardware:** `PM4_XE_SWAP` (0x64) is Xenia's own (xenos.h:1624). The driver must not use it.
- **Indirect buffers:** the ring can stay small with indirect buffers, with per-frame command buffers in physical
  memory, as D3D does.

### 3.4 Render targets, eDRAM and resolves

- **Drawing into eDRAM.** Xenos draws into 10 MB of eDRAM. The surface and the colour and depth targets are set by
  `RB_SURFACE_INFO` 0x2000, `RB_COLOR_INFO` 0x2001 and `RB_DEPTH_INFO` 0x2002
  (xenia:src/xenia/gpu/register_table.inc:480-482), plus `RB_MODECONTROL` 0x2208, `RB_COLOR_MASK` 0x2104,
  `RB_DEPTHCONTROL` 0x2200, `RB_BLENDCONTROL0` 0x2201, the window scissor 0x2081-0x2082, the viewport from 0x210F and
  `PA_SU_SC_MODE_CNTL` 0x2205 (register_table.inc:498-544).
- **Resolving to memory.** A "resolve" copies eDRAM to memory, optionally clearing it, under `RB_COPY_CONTROL` 0x2318
  and `RB_COPY_DEST_BASE`, `_PITCH` and `_INFO` 0x2319-0x231B (register_table.inc:595-598). Xenia derives a resolve
  from these in `GetResolveInfo` (xenia:src/xenia/gpu/draw_util.h:481, 599-604), the reference for the exact
  register values.
- **Presenting.** The front buffer the display controller shows is such a resolve target in memory, in the
  display's 32x32 tile layout (docs/HARDWARE.md).
- **The tiling decision:** whether the frame fits in eDRAM at once or needs Xenos's predicated tiling decides early
  complexity.
  - 1280x720 with 32-bit colour and 32-bit depth, no MSAA, is about 7 MB and fits.
  - Recommendation: no MSAA and no tiling in the first milestones.

### 3.5 Textures, vertices and constants

- **Texture fetch constants:**
  - 6 dwords each (`xe_gpu_texture_fetch_t`, xenia:src/xenia/gpu/xenos.h:1184-1285): base address, format,
    dimensions, swizzles, endian mode, tiling and mip address;
  - formats are the `TextureFormat` enum (xenos.h:450);
  - they live in the fetch-constant registers from 0x4800 (register_table.inc:2697).
- **Tiled textures** use Xenos's own address swizzle (`Tiled2D`, `Tiled3D`,
  xenia:src/xenia/gpu/texture_address.h:174-237). The SDK needs a tiling routine for uploads. Linear textures are
  allowed and simpler for a first textured quad.
- **Vertex fetch constants** are 2 dwords (`xe_gpu_vertex_fetch_t`, xenos.h:1113-1127), in the same fetch-constant
  space.
- **Float shader constants** start at 0x4000 (`SHADER_CONSTANT_000_X`, register_table.inc:648). They're set by type-0
  packets or `SET_CONSTANT`, or loaded from memory with `LOAD_ALU_CONSTANT`.

### 3.6 Shaders under the SDK

- **Loading.** The driver loads a vertex and a pixel program with `IM_LOAD`. The first dword is the physical address
  with the shader type in its low 2 bits; the second is the start (0) and the size in dwords
  (xenia:src/xenia/gpu/command_processor.cc:1528-1556).
- **Setup.** It writes `SQ_PROGRAM_CNTL` 0x2180 and `SQ_CONTEXT_MISC` 0x2181 (register_table.inc:523-524), from the
  values xost's container already carries (xost:docs/container.md:28-37).
- **Two pieces xost lacks for this target:**
  - A loader that takes a xost container, places its microcode in physical memory, and returns the register values.
    That's what libxenon's `Xe_LoadShaderFromMemory` did.
  - The vertex-fetch patch: rewriting the template `vfetch` instructions for the bound vertex format, as
    `Xe_ShaderApplyVFetchPatches` did (xost:docs/container.md:48-55). Alternatively, xost could emit vertex shaders
    already specialised to a vertex format, so no patching is needed.
- **The 16-register ceiling still applies:** too many registers silently hangs the console, as it did under libxenon
  (xost:docs/HANDOFF.md:31-36).

## 4. Milestones

Each is proven on the user's console, as xost's milestones were.

1. **A `.xex` that runs from the dashboard.** Done when:
   - `elf2xex` turns a GCC ELF into a XEX2 with a valid PE image, imports, TLS and an entry point;
   - it launches from the dashboard, writes a log file to USB or HDD, and returns to the dashboard with
     `XamLoaderTerminateTitle`;
   - the notes record which XEX checks the console's kernel enforces (section 2.2).
2. **The runtime.** Done when a test title:
   - runs two threads that hand work over through events and a semaphore;
   - reads and writes files through newlib;
   - logs a controller's state from `XamInputGetState`;
   - plays a tone through the audio render-driver client.
3. **The C++23 runtime** (section 2.5). Done when a test title built with `-std=c++23`, logging each result:
   - runs static constructors before `main`, and allocates aligned and over-aligned objects;
   - runs `std::jthread`s that share a `std::mutex` and a `std::condition_variable`, and reads `thread_local` values
     that differ per thread;
   - counts with `std::atomic` (32- and 64-bit), and wakes a thread through `std::atomic::wait`/`notify_one`, with the
     waiting thread measured to sleep rather than spin;
   - throws and catches an exception, including one thrown on another thread and passed back through
     `std::exception_ptr`, and uses `dynamic_cast`;
   - times a known delay with `std::chrono::steady_clock`, and prints the date from `system_clock`;
   - lists, creates and removes a directory with `std::filesystem`;
   - writes its results with `std::println`, and uses `std::expected` and `std::format`.

   `std::stacktrace` with names is a later item (section 2.5).
4. **The command processor.** Done when:
   - the title allocates write-combined physical memory and initialises the ring and the read-pointer write-back;
   - it submits `NOP` and an `EVENT_WRITE` fence and sees the fence value land in memory;
   - the microcode question (section 3.2) is answered.
5. **A cleared screen.** Done when the title clears an eDRAM colour target, resolves it to a front buffer in memory,
   and presents it through the display controller, every frame, in a colour that changes over time. The kernel's
   `VdSwap` doesn't flip on the user's console (docs/HARDWARE.md). The front buffer was 640x480, the kernel's mode
   scaled to the output; 1280x720 came with item 10.
6. **A triangle.** Done when a xost vertex and pixel shader, loaded by the SDK's own loader with `IM_LOAD`, draw a
   coloured triangle from a vertex buffer, with the vertex format set by fetch constants.
7. **A textured quad.** Done when a texture uploaded by the SDK samples correctly: linear first, then tiled through the
   SDK's `Tiled2D`.
8. **Render to texture.** Done when a scene resolves to a texture that a second pass samples. A translation layer
   relies on exactly this.
9. **Frame pacing.** Done when frames are paced to vsync through the interrupt callback and fences, with two frames in
   flight and no stalls or tearing over a 10-minute run.
10. **720p**, agreed after milestone 9. Done when titles get a 1280x720 front buffer by default, which the display's
    scaler fits to the console's output (1:1 at 720p, 1.5x at 1080), and can still ask for the kernel's 640x480; the
    GPU tests pass at 1280x720 on hardware, a capture shows the scaled test image whole, and the console goes back to
    Aurora. Interlaced outputs keep the kernel's mode until scaling there is tried.

After milestone 9, the SDK and the GPU layer are a complete base. A later translation layer can build on them: render
passes on eDRAM and resolves, descriptors on fetch and shader constants, and shaders from xost's GLSL path. That layer
is vulkan4xenon, scoped in its own repository's docs/ROADMAP.md (Z:\vulkan4xenon).

### GPU milestones before a Vulkan translation layer

Agreed after item 10, in this order, on 2026-10-10.

**Why these are needed.** The GPU layer's pipeline is fixed today:
- one 32-bit colour target, with no depth, blending or culling;
- non-indexed triangle lists from one stream of float vertices;
- point-sampled, clamped 32-bit textures with no mips;
- no way to set shader constants.

A Vulkan-style layer needs that state settable.

**The order** follows what such a layer would hit first. All eight come before the layer is scoped: 16 to 18 were to
run alongside its early work, and were moved ahead of it on 2026-10-11, after milestone 15.

**How state is built.** Each piece of state is a precomputed block of register writes, so a pipeline can build its
state once and apply it when bound. These are not more convenience calls (the GPU layer stays low-level, as agreed).

**When each is done.** On a clean hardware run of tests whose variants are distinguishable on a capture, and which fail
when broken on purpose.

11. **Shader constants.** Done when:
    - a title sets vertex and pixel shader float constants per draw (`SHADER_CONSTANT_000_X` from 0x4000,
      xenia:src/xenia/gpu/register_table.inc:648);
    - it sets boolean and loop constants (from 0x4900 and 0x4908, register_table.inc:2890, 2899);
    - a test draws several quads in one frame from the same shaders, each placed and coloured only by its constants,
      checked by pixel;
    - the test also takes one quad down a path chosen by a boolean constant, which xost hasn't proven on hardware
      (xost:README.md:89-91).
12. **Indexed draws and topologies.** Done when:
    - titles draw from 16- and 32-bit index buffers in memory (`PM4_DRAW_INDX` with `VGT_DMA_BASE` and `VGT_DMA_SIZE`,
      xenia:src/xenia/gpu/command_processor.cc:1401-1415, register_table.inc:531-532; `IndexFormat`,
      xenia:src/xenia/gpu/xenos.h:209);
    - they draw triangle strips, line lists and strips, and point lists (`PrimitiveType`, xenos.h:44);
    - a test draws the same shape each way side by side, each in its own colour, checked by pixel.
13. **Depth and stencil.** Done when:
    - the target can carry a 24-bit depth, 8-bit stencil buffer in eDRAM (`RB_DEPTH_INFO`,
      xenia:src/xenia/gpu/registers.h:835; `DepthRenderTargetFormat`, xenos.h:367), cleared to a value;
    - depth test and write and the stencil test are set per draw (`RB_DEPTHCONTROL` and `RB_STENCILREFMASK`,
      registers.h:799-834);
    - a test draws overlapping shapes back to front and front to back, and a shape masked by stencil, checked by pixel;
    - the same draws with the depth test off fail the depth check.
14. **Raster and blend state.** Done when:
    - each of these is set per draw from a state block:
      - blend factors and operations (`RB_BLENDCONTROL0`, registers.h:780; `BlendFactor`, xenos.h:708);
      - the colour write mask (`RB_COLOR_MASK`, register_table.inc:505);
      - culling and front face (`PA_SU_SC_MODE_CNTL`, registers.h:456);
      - the scissor (`PA_SC_WINDOW_SCISSOR_TL` and `_BR`, registers.h:622);
      - the viewport (`PA_CL_VPORT_*` from 0x210F);
      - depth bias (`PA_SU_POLY_OFFSET_*` from 0x2380, register_table.inc:615);
    - a test draws one variant of each side by side, checked by pixel against values worked out on the CPU.
15. **Sampling and mipmaps.** Done when:
    - textures carry mip levels (the mip address and packed mips in the fetch constant, xenos.h:1184-1285);
    - samplers choose point or linear filtering for magnification, minification and mips (`TextureFilter`,
      xenos.h:131);
    - they choose wrap, mirror or clamp addressing (`ClampMode`, xenos.h:104);
    - a test draws the same quad under each setting side by side, each mip level its own colour, checked by pixel.

    Anisotropic filtering waits.
16. **Texture formats.** Done when:
    - textures upload and sample, linear and tiled, in DXT1, DXT2/3 and DXT4/5 (Vulkan's BC1 to BC3), 5-6-5, 4-4-4-4,
      1-5-5-5, 8, 8-8, 2-10-10-10 and 16-bit float (`TextureFormat`, xenos.h:450);
    - each format has its own endian swap;
    - a test shows one quad per format, each a distinct pattern, checked by pixel against a decode on the CPU.
17. **Vertex input.** Done when:
    - a draw takes several vertex streams with their own strides (vertex fetch constants, xenos.h:1113-1127);
    - attributes come in byte, short, normalised and half-float formats as well as float (`VertexFormat`, xenos.h:602),
      through the vertex-fetch patch;
    - a test draws one shape per format, each in its own colour, checked by pixel.

    Byte (ubyte4) formats are unproven on hardware (xost:README.md:89-91).
18. **Recorded command buffers.** Done when:
    - packets can be recorded into memory ahead of time and run from the ring with `PM4_INDIRECT_BUFFER` (section 3.3;
      command_processor.cc:731-733);
    - a buffer can run more than once, and several can run in a frame;
    - a test records a frame's draws once and replays them for many frames, paced, under milestone 9's checks.

**Left out of these, for when the layer needs them:**
- multiple render targets, unproven in xost;
- float render targets and multisampling;
- predicated tiling, for targets larger than eDRAM;
- cube and 3D textures, and anisotropic filtering;
- occlusion queries;
- depth resolves, and resolving part of a target.

### Draw throughput, after vulkan4xenon's benchmark

Agreed on 2026-10-11, after vulkan4xenon's milestone 5 measured the GPU layer on the console
(vulkan4xenon:results/2026-10-11-milestone-5/vkbench_run1.log).

**Why.** A cube drawn with a matrix and a tint cost 37.6 us through the GPU layer, and 4000 of them took 151 ms a
frame. The GPU finished each frame's last commands and its resolve 0.3 ms after the submit, so it sat idle; the CPU
waited for room in the ring.
- A draw writes 156 dwords: each constant write sends the whole block of 16 registers around it, and each indexed draw
  carries a coherency request. The 32 KB ring fills every 52 draws.
- Once full, the ring is kicked and the CPU sleeps in 1 ms steps, which come to about 2 ms on the console.
- libxenon records into a 4 MB buffer through the CPU's cache, flushes it, and kicks every 4 KB of it from the ring
  with `PM4_INDIRECT_BUFFER` (libxenon:drivers/xenos/xe.c:21-23, 52-75, 108-147). It invalidates the GPU's caches when
  the CPU unlocks a buffer, not at every draw (xe.c:750-761).

19. **Draw throughput.** In three steps, each run on the console:
    1. **Measure.** The ring counts what it writes, its kicks, its waits for room and the time they take, and a title
       reads them. vkbench logs them for each scene, with a 1 ms sleep timed and the CPU's write speed into
       write-combined memory against cached memory it then flushes.
    2. **Record ahead of the GPU.** Commands go into a large buffer, which the ring runs in segments with
       `PM4_INDIRECT_BUFFER` as they fill, as libxenon does, so the GPU works while the CPU records. When the GPU is
       behind, the CPU waits without 1 ms sleeps. Step 1's numbers choose between cached and write-combined memory.
    3. **Fewer dwords a draw.** Constants go in smaller groups than 16 registers, where the console takes them: each
       group size gets its own hardware test, as three float constants alone hung the GPU (src/gpu_constants.c).
       Coherency requests cover only memory the CPU wrote, which the title declares.

    Done when, on the console:
    - vkbench's `draws_1000` through the GPU layer fits a 60 Hz frame: draw and submit in under 16 ms, under 16 us a
      draw;
    - `draws_4000` is recorded, with the counters showing where its time goes;
    - xsdk_test and vulkan4xenon_test pass in full, and vkbench's two backends still draw the same pictures.

### Memory copies, after milestone 19

Agreed on 2026-10-11, after milestone 19's probes timed the CPU's writes on the console (docs/HARDWARE.md).

**Why.** newlib's `memcpy` wrote 1 MB into write-combined memory at 185 MB/s, where volatile 32-bit stores in order
wrote it at 3603 MB/s. As the toolchain builds it, its loop copies 16 bytes with four 32-bit loads and stores, the
stores scheduled out of order, and it copies data that isn't 4-byte aligned a byte at a time. Vertex streams and the
GPU layer's uploads go through it into write-combined memory, and so do Vulkan apps filling mapped memory.

20. **Memory copies.** In two steps, each run on the console:
    1. **Probe.** vkbench times 1 MB copied into write-combined memory with newlib's `memcpy`, in-order 32-bit stores,
       aligned 64-bit stores and VMX 128-bit stores, and between cached buffers with VMX and `dcbt`.
    2. **The SDK's own `memcpy` and `memmove`,** which titles link ahead of newlib's: stores in order, in the width step
       1 found fastest, and unaligned copies without a byte loop. No `dcbz`, which works only in cached memory, so they
       are safe on any memory.

    Done when, on the console:
    - `memcpy` writes write-combined memory at 3 GB/s or more;
    - vkbench's `stream_8000` draw time falls to match, from 5.5 ms;
    - xsdk_test, vulkan4xenon_test and vkbench pass in full.

### What Dusklight needs from the platform

Agreed on 2026-10-11 with vulkan4xenon's milestone 9, Dusklight on Aurora's null backend
(vulkan4xenon:docs/ROADMAP.md).

**Why.** Dusklight and encounter's Aurora build with CMake, and reach the console through SDL3, which gets an xsdk
platform for the port. What they need of the SDK beyond what it has:
- a way to build with CMake: Aurora and Dusklight are CMake projects, and the SDK builds titles only with its Makefile;
- a heap past 256 MB: the game asks for 280 MB of GameCube memory and ARAM before Aurora's pools
  (dusklight:src/m_Do/m_Do_main.cpp:705-706), and `sbrk` reserves 256 MB (src/sbrk.c);
- what SDL3's threads and the game's ask of pthreads beyond the SDK's today, settled as SDL's thread backend is built
  against it: stack sizes, priority and affinity, semaphores.

21. **The platform for Dusklight.** Each piece as the port reaches it, each with a test in xsdk_test:
    - **A CMake toolchain file** that builds and links a title as the Makefile does: `-fno-pic`, `-static`, `src/xsdk.ld`,
      `libxsdk.a` (and `crt0.o`, until milestone 22 retired it), and `elf2xex` after the link;
    - **the heap's reservation** sized from what the console leaves a title, measured first;
    - **the pthread pieces** SDL3 and the game use.

    Done when, on the console:
    - Dusklight's `.xex` built through the toolchain file starts, and reaches milestone 9's steps;
    - xsdk_test passes in full, with a test for each piece.

### The console's calling convention

Agreed on 2026-10-12, after the SDL3 port. The user chose on 2026-10-11 to keep GCC's struct layout, so code ported
meanwhile needn't change (TODO.md), and allowed the Xbox 360 SDK's compiler for the probe's MSVC half throughout.

**Why.** The kernel, XAM and any library built with the console's compiler call by MSVC's convention, and the SDK's
GCC by 32-bit SysV: on the console, 27 of 36 cases failed between them (docs/HARDWARE.md, "Calls between xsdk's GCC
and the console's MSVC"). The SDK keeps today's calls inside where they agree:
- the import stubs move arguments past the 8th into the kernel's slots;
- `XSDK_KERNEL_ENTRY` saves r14-r31 whole around the kernel's calls into a title;
- `-ffixed-v14` to `-ffixed-v19` keep GCC off vector registers the kernel's code keeps;
- `gen_imports.py` refuses the rest.

Two kernel callbacks run unwrapped all the same and lose r14-r31's high halves (src/present.c:57, src/gpu.c:107). A
GCC that calls as MSVC does lets an import take its natural declaration, and the kernel call a title's code directly.

**The convention,** as the console showed it, settled by step 1 (docs/HARDWARE.md, "Calls between xsdk's GCC and
the console's MSVC" and "The console's calling convention, register by register"):
- **Arguments:** one 8-byte slot each, r3 to r10, then the stack from 0x50(r1). The GPRs are counted apart from the
  slots: a struct of more than 8 bytes starts on a 16-byte boundary, an even slot, while its GPRs, and those of what
  follows, come from the GPR count, which that padding doesn't move (step 2, docs/HARDWARE.md).
  - A 32-bit value goes in its slot's low word, its high half carrying nothing: the receiver widens it.
  - 64-bit integers go whole.
  - `float` and `double` go in f1 to f13, each also taking a slot whose register is left alone; from the 14th, in
    their slots, a `float` as single precision in the low word.
  - Structs go as their bytes: 1, 2 and 4 bytes right-justified, other sizes from the first slot's high byte on.
- **Returns:** 64-bit integers, and structs of 1, 2, 4 and 8 bytes, in r3; `float` and `double` in f1; other structs
  through a pointer in r3, which comes back in r3.
- **Extension:** whoever receives a `char`, `short` or `int` widens it.
- **Varargs:** each in its own slot, doubles as their bits, also copied into f1 on by the caller. `va_list` is a
  pointer that walks the 8-byte slots; the callee spills the GPRs left into the slots after its named arguments,
  which are the 64 bytes at 0x10(r1) that its caller always leaves unless a padded struct came before.
- **Registers kept:** r14-r31 whole, f14-f31 and v14-v31 (and VMX128's v64-v127, which GCC can't reach); no CR field.
- **Frames:** LR goes at -8(r1) through r12, and saved registers go below the caller's r1 before `stwu`; nothing is kept
  at 4-15(r1). The 1348 bytes below r1 are the code's: the console's own writes there start below them.

Layout stays GCC's: `ms_struct` and `char16_t` where code shares structs or text with MSVC code.

**How.** An option in GCC's PowerPC backend, on the 32-bit target the SDK has, not a 64-bit one. Under
`-m32 -mpowerpc64` GCC already works in 8-byte words and keeps a `long long` in one register
(gcc:config/rs6000/rs6000.h:646, rs6000.cc:1837-1856); only the calling convention is 4 bytes wide. The option,
`-mcall-xsdk`, becomes the toolchain's default. It sits on the SysV ABI the target has (no TOC, r13 the processor
control region) and takes the paths GCC's AIX ABI already has for one slot per argument, f1 to f13 doubling integer
slots and structs by value (rs6000-call.cc:1245-1261, 1781-1869). On top of those paths:
- argument slots of 8 bytes, and a parameter area at 0x10 with its 64 bytes always left (rs6000-call.cc:890-903,
  rs6000.h:698, 1257-1259);
- no 64-bit value split across two registers, as an argument or a return (rs6000-call.cc:1478-1521,
  rs6000.cc:23994-24006), and 8-byte structs returned whole in r3;
- no extending by the sender (rs6000-call.cc:693-704);
- r14-r31 saved 64 bits wide, so GCC may keep 64-bit values in them across calls (rs6000.cc:2016-2023,
  rs6000-logue.cc:2997-2998);
- v14-v31 kept (rs6000.cc:10232, rs6000-logue.cc:167-170), which retires `-ffixed-v14` to `-ffixed-v19`;
- MSVC's frame, with LR at -8(r1);
- an unwinder that restores 8-byte registers, so a C++ exception gives them back whole (`rs6000_dwarf_frame_reg_mode`
  in tools/toolchain/gcc-xsdk.patch; libgcc's unwind-dw2.c);
- objects flagged by the assembler, `EF_PPC_XSDK`, which the linker refuses to mix with objects built for another
  convention (tools/toolchain/binutils-xsdk.patch; binutils:bfd/elf32-ppc.c, `ppc_elf_merge_private_bfd_data`).

Not chosen: 32-bit pointers on GCC's 64-bit mode.
- Its objects are ELF64, which the SDK's ELF32 link and `elf2xex` don't take.
- It means auditing some 640 uses of the pointer mode.
- Nothing in GCC does it; AIX's port refuses outright (gcc:config/rs6000/aix73.h:39-42).

The runtime follows:
- libgcc's out-of-line save and restore routines take MSVC's shape;
- newlib's `setjmp` saves r14-r31 whole and v14-v31, in a larger `jmp_buf`.

So does the SDK:
- the import stubs become their thunks, and `check_abi` goes;
- `XSDK_KERNEL_ENTRY` goes, with its trampolines in crt0, threads, the audio tests and SDL's audio driver.

22. **The console's calling convention.** In three steps, each run on the console:
    1. **What the probe left open.** The probe gains the cases its first run didn't have:
       - structs of 2 to 23 bytes, as arguments and returns, and the arguments after them;
       - `float` and `double` returned;
       - floating-point arguments past the 13th;
       - varargs with `float`s, and past 8 slots;
       - 32-bit arguments with their high halves set;
       - the CR fields MSVC's callers keep across a call;
       - what MSVC keeps at 4 to 15(r1), and how far below r1 it writes;
       - MSVC code that keeps f14-f31 and v14-v31.

       Its MSVC half is built with the Xbox 360 SDK's compiler, which the user allowed for it; its output stays in
       `local/`, out of git. The answers go into docs/HARDWARE.md and settle the convention above.
    2. **The compiler, and the SDK on it.** The option, libgcc and newlib are built into a prefix of their own,
       `~/xsdk-gcc-next`. Meanwhile `~/xsdk-gcc` keeps the SysV toolchain, which Dusklight's port builds with.
       - On a branch, the SDK's pieces that must change with the compiler: the import stubs, `XSDK_KERNEL_ENTRY` and
         its users, and the kernel-entry test. That test then checks that GCC code called directly keeps r14-r31
         whole, f14-f31 and v14-v31.
       - Objects built for the two conventions don't link together unnoticed.
    3. **The switch.** `~/xsdk-gcc` becomes the new toolchain, with the old one kept beside it. SDL drops its audio
       trampoline, and SDL, vulkan4xenon, Aurora and Dusklight are rebuilt.

    Done when, on the console:
    - the probe built with the new compiler passes every case, the first run's 36 and step 1's, in both directions;
    - xsdk_test passes in full, with new checks:
      - a C++ exception thrown through frames that hold 64-bit values in r14-r31, and values in f14-f31 and v14-v31,
        gives them back;
      - `setjmp` and `longjmp` keep them;
      - `KeQueryPerformanceFrequency`, declared with its natural 64-bit return, reads 50 MHz;
    - sdl_check, vulkan4xenon_test and vkbench pass in full;
    - Dusklight's port passes its latest step again.

23. **Floating point at any address.** Dusklight found it on the console (docs/HARDWARE.md, "Dusklight runs the title
    scene"): a float load from an address that isn't a multiple of 4 raised an alignment exception, 0x80000002, where
    integer loads from the same stream ran. Xenia doesn't check alignment. GCC puts float-register loads and stores on
    unaligned memory under both of xsdk's toolchains, the SysV one and `-mcall-xsdk`'s:
    - `memcpy` into a `float` or `double`, or into an integer that `std::bit_cast` then turns into one, becomes `lfs`
      or `lfd` at the source: GCC's `combine` folds the integer load and the conversion into one float load that still
      records the source's byte alignment;
    - a `double` stored to a packed struct becomes `stfd` at its offset: the expander stores it as a 64-bit integer,
      and the register allocator keeps that integer in a float register.

    `-mstrict-align` stops them, but it also makes every unaligned integer access go byte by byte through the stack,
    where today one `lwz` reads it and the console takes it.

    So on xsdk's target, GCC moves a `float` or `double` between a float register and memory only where it knows the
    memory is aligned to the value's size; any other goes through the integer registers, which take unaligned
    addresses as they do now. In rs6000: a memory constraint on the float-register alternatives of the move and
    conversion patterns, and `rs6000_legitimate_combined_insn` refusing what `combine` folds otherwise.

    Done when:
    - an option that makes each such access a compile error finds none in xsdk, SDL, vulkan4xenon, Aurora and
      Dusklight;
    - xsdk_test writes floats and doubles to every offset from 0 to 7 and reads them back, through `memcpy`,
      `std::bit_cast` and packed structs, and gets every value on the console; built with today's compiler, the same
      check faults there;
    - xsdk_test, sdl_check, vulkan4xenon_test, vkbench and Dusklight's title scene pass on the console, with vkbench's
      times within the noise of milestone 22's run.

24. **Pixel shaders past 16 registers.** A pixel shader that uses r16 or above hangs the GPU, so xost stops at 16
    (xost:xasm/program.py:453-457, "r16 hangs, r15 runs"), and Dusklight's title scene has a program that needs 18,
    left out of its shader pack (vulkan4xenon:docs/ROADMAP.md, milestone 10). The 16 is xsdk's set-up, not the GPU:
    - `SQ_GPR_MANAGEMENT` splits the shader unit's registers between the two kinds of shader, pixel shaders' share in
      bits 4-11 and vertex shaders' in bits 12-19 (Linux:drivers/gpu/drm/msm/registers/adreno/a2xx.xml:1231-1235,
      Adreno 2xx's register file, which has it at Xenos's 0x0D00, xenia:src/xenia/gpu/register_table.inc:139);
    - the kernel leaves it at 0x00040400, 64 and 64, and xsdk's set-up, libxenon's, steps it from 0x10 to 0x70 and
      ends at 0x00070100: 16 for pixel shaders and 112 for vertex shaders (docs/HARDWARE.md, "libxenon's set-up makes
      the resolves work"; libxenon:drivers/xenos/xe.c:443-452, 527-528).

    So:
    - xsdk's set-up steps the split as libxenon does, since which part of the set-up makes the resolves work isn't
      isolated, but stops at the kernel's 64 and 64 (src/gpu_setup.c, `put_register_split`);
    - xost takes each kind of shader's register budget as an option, defaulting to libxenon's 16 and 112 for its own
      apps, and refuses a shader over it, where today it only warns that a pixel shader will hang; xsdk's and
      vulkan4xenon's shader builds pass 64 and 64;
    - Dusklight's shader pack is rebuilt with the program it left out (Dusklight's fork,
      `platforms/xenon/glsl_pack.c`).

    Done when:
    - xsdk_test draws with pixel shaders that use 18 and 64 registers and a vertex shader that uses 64, each carrying
      a value through every register it uses into the colour, and reads back the right colours on the console; built
      with libxenon's split, the same check fails there, the 18-register draw's fence never landing;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console, with vkbench's GPU times compared with milestone
      23's run, as the split changes how the shader unit shares its registers;
    - Dusklight's pack holds all 69 programs recorded, and its title scene runs on the console without leaving a
      program out for its registers.

25. **Shaders' microcode shares pages.** `xsdk_gpu_alloc` gives every allocation 64 KiB pages of its own (src/gpu.c,
    `allocate`), and `xsdk_shader_load` takes one for each shader's microcode (src/shader.c), which vulkan4xenon loads
    again for each pipeline (vulkan4xenon:src/pipeline.c, `load_shaders`). Counted in Xenia on 2026-10-12, Dusklight's
    boot screens and a minute of its title scene made 87 GPU allocations, 73 of them under a page and 70 of those 24 to
    1,440 bytes, shaders' microcode: about 85 KB in 73 pages, 4.6 MB (`results/2026-10-12-gpu-allocations/`). The
    title scene ends with 9 MB free, and vulkan4xenon's milestone 11 adds about 60 pipelines, 7.5 MB more at a page a
    shader. Decided with the user on 2026-10-12: microcode only, as the other small allocations would save a few pages.

    So:
    - a shader's microcode comes from shared 64 KiB pages in 256-byte blocks, the alignment libxenon gives it
      (libxenon:drivers/xenos/xe.c:943); `xsdk_shader_free` gives its blocks back, and a page goes back to the kernel
      with its last block;
    - under a lock, as Aurora loads shaders from two threads; loading a shader isn't on the hot path.

    Done when:
    - xsdk_test loads 200 small shaders, which take a few pages where they took 200 by the kernel's count, and draws
      with each right on the console; broken on purpose, two shaders handed overlapping blocks, the check fails;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console;
    - Dusklight's title scene ends with about 4 MB more free than at vulkan4xenon's milestone 11, step 2.

26. **Threads on all six hardware threads.** A thread the SDK starts stays on its creator's hardware thread. On the
    console on 2026-10-13 (`results/2026-10-13-threads-probe/`, tests/probe_threads.c), a title's main thread ran on
    hardware thread 0 and so did every thread it started: two busy ones got half of one thread's work done each, six
    about a fifth. Dusklight starts about twenty, all on that one hardware thread, and its title scene ran at 17 to 22
    fps (vulkan4xenon:docs/ROADMAP.md, milestone 11, step 5). Placed, they run apart: `KeSetAffinityThread` moved a
    running thread and `ExCreateThread`'s processor bits started one where asked; two busy threads on two cores got 100%
    and 93% done, and on one core's two hardware threads half each. Decided with the user on 2026-10-13: the kernel's
    placement stays the default, and a title places the threads it knows of.

    So:
    - xsdk starts a thread on a given hardware thread (`ExCreateThread`'s processor bits,
      xenia:src/xenia/kernel/xthread.cc:355-356), moves the calling thread to one (`KeSetAffinityThread`,
      xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:240-256), and says which one a thread runs on (the
      processor control region at r13, xenia:src/xenia/kernel/xthread.h:81-83);
    - Aurora places its threads by name, from a list a game can change, and Dusklight places its own. To begin with:
      the game thread on hardware thread 0; the GPU waiter, audio and the game's threads on 1; the render worker on 2;
      texture preconversion and pipeline compiles on 3; the FIFO processor on 4; the decode helper on 5.

    Done when:
    - xsdk_test starts a thread on each hardware thread and moves one to each, and finds it there; an unplaced thread
      runs where its creator does; two busy threads on two cores each get at least 80% of one's work done, and on one
      hardware thread at most 60%; broken on purpose, with the placement ignored, those checks fail;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console;
    - Dusklight's title scene runs on the console with each placed thread where the list puts it, and its frame rate is
      measured against step 5's 17 to 22 fps.

27. **The log, read while the title runs.** xsdk opens a title's log with no share access (src/log.c), so on the
    console nothing else can open it until the title ends: opened for reading meanwhile, it fails with a sharing
    violation. A tool following a title's log as it runs needs that. Decided with the user on 2026-10-13.

    So:
    - xsdk opens the log shared for reading (`NtCreateFile`'s share access; the values are NT's, and Xenia ignores it,
      xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:102-145);
    - `xsdk_log_path` says where the log is.

    Done when:
    - xsdk_test opens its own log for reading while it's open and finds its first line; broken on purpose, with the
      log opened unshared, the check fails on the console;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console.

28. **32-bit float textures.** Dusklight's range fog works out, for every pixel, a factor that depends only on the
    pixel's column: about 2.4 ms of the title's logo shot's 22 ms of GPU time a frame, measured with it taken out
    (vulkan4xenon:docs/ROADMAP.md, milestone 11). A row of 32-bit floats, one a column, holds it exactly, read with
    one fetch; xsdk's formats stop at half floats. Decided with the user on 2026-10-13, as step 2 of the shader work.

    So:
    - `XSDK_FORMAT_R32F`, the TextureFormat `k_32_FLOAT` (xenia:src/xenia/gpu/xenos.h:512), 32-bit units swapped
      8-in-32 like the other 32-bit formats, read as red with green and blue 0 and alpha 1.

    Done when:
    - xsdk_test draws an R32F texture, linear and tiled, and reads back the values written, 0 to 1 in eighths and
      past both ends clamped; broken on purpose, with its units unswapped, the check fails on the console;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console.

29. **Uploads in runs, and channel swizzles.** Dusklight's attract demo plays its movie at 7.5 fps: each frame's three
    textures, 1.6 MB, take vulkan4xenon's copies 114 ms, and xsdk_texture_upload works out each texel's tiled offset
    on its own and merges a texel under 4 bytes into its word by reading the word back from write-combined memory
    (vulkan4xenon:docs/ROADMAP.md, "Then the memory for the attract demo"). The movie's 8-bit textures, expanded to
    32 bits now, are to stay 8-bit and sample as intensity, which needs a texture's channels picked. Decided with the
    user on 2026-10-13, with vulkan4xenon's milestone 12 and Aurora's side.

    So:
    - `xsdk_texture_upload` writes runs of blocks that sit together in memory with one offset each, as whole words:
      in Xenos's tiled layout 16 bytes or 8 texels of a byte (xsdk_tiled_2d), a linear level's whole row. Blocks a
      run doesn't cover, at a packed mip level's edges, go as now.
    - `xsdk_texture_swizzle` sets which of the format's channels, or 0 or 1, the GPU returns as red, green, blue and
      alpha: the fetch constant's swizzle (xenia:src/xenia/gpu/xenos.h:1015-1030), composed with the format's own.
    - Found on the way, by vulkan4xenon's milestone 12 test and probe_mip_tail with thin shapes: the console reads a 2x1
      texture's packed base at (16, 0), as a square's, where xsdk, after Xenia, put it at (0, 16); 4x1 and wider are
      laid out as Xenia has them. xsdk places 2x1 as the console reads it.

    Done when:
    - xsdk_test's formats check passes with the uploads in runs, every format linear and tiled and every mipped level
      as before, and draws R8 swizzled to red in every channel and R8G8 to red, red, red then green, every pixel as the
      CPU decodes it; broken on purpose, a swizzle left at the format's, that check fails;
    - xsdk_test times a 512x512 R8 tiled upload from cached memory, which must take under 4 ms; broken on purpose, the
      blocks written one at a time as before, it fails on the console;
    - probe_mip_tail reads 2x1, 4x1 and 1x2 textures where xsdk puts them, and vulkan4xenon_test's 2x1 R8 image draws
      its texels;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console.

Milestones 30 to 35 come from measuring what the console does through xsdk, CPU, memory and GPU, against the published
figures for the hardware (500 MHz, 8 pixels and 16 bilinear samples a clock). Decided with the user on 2026-10-13: each
gap that something running on xsdk meets becomes a milestone; the rest wait (after 35).

30. **Hierarchical depth.** xsdk keeps hierarchical depth off (`RB_HIZCONTROL` 0, src/gpu_setup.c), and a pixel
    behind others costs what a drawn one does: full-screen quads behind a near one, through a 64 multiply-add pixel
    shader, ran at 0.363 Gpixel/s, the rate the same shader draws at. Xenia says Xbox 360 titles never set an early
    depth bit (xenia:src/xenia/gpu/registers.h:805), so hierarchical depth is the console's only rejection ahead of
    the pixel shader. Dusklight draws a great deal that ends up behind other things. Xenia names `RB_HIZCONTROL`
    (0x2203), `RB_HIZ_CLEAR` (0x231C; xenia:src/xenia/gpu/register_table.inc:539, 599) and `RB_SURFACE_INFO`'s HiZ
    pitch (registers.h:676), which libxenon sets (libxenon:drivers/xenos/xe.c:1019, 1033), but not `RB_HIZCONTROL`'s
    fields.

    So:
    - The fields, and what keeps the hierarchical buffer right through depth clears and depth writes, from public
      reverse engineering where it has them and probes on the console where it doesn't.
    - xsdk turns it on for draws that test depth, and keeps it right across resolves that clear depth, depth writes
      and compare changes, with no call from the title; where a setting can't use it, xsdk leaves it off.

    Done when:
    - a probe times hidden pixels, quads behind a near one through a long pixel shader, with hierarchical depth on and
      off, and on is many times faster;
    - xsdk_test's depth checks pass with it on, and a check draws near and far geometry in both orders, across depth
      clears, every pixel as the CPU works it out; broken on purpose, the hierarchical buffer left uncleared when depth
      clears, that check fails on the console;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console, and Dusklight's logo shot draws as before.

31. **The pixel rate.** A full-screen quad through a one-instruction pixel shader with no registers fills 2.0
    Gpixel/s through xsdk, 4 pixels a clock, opaque, blended and depth-only alike; small triangles of 8 pixels reach
    166 million a second. The pixel ALUs reach their published peak (186 GFLOPS of 192 for 4-lane multiply-adds), so
    the shader isn't it: some part of the draw state xsdk takes from libxenon holds the pixel rate to half of 8 a
    clock. Point and bilinear sampling, 4 samples a pixel, reach 2.6 Gtexel/s, a third of 16 a clock; xost's sampling
    shader holds 12 registers, which xost's own work takes on (xost:ROADMAP.md), and this milestone's answer may be
    part of that too. Every title's pixels go through this.

    So:
    - A probe that times a fill, a sampled fill and a resolve while it changes one piece of xsdk's draw state at a
      time: the shader register split, the sequencer's thread and export set-up, the surface, mode and colour
      registers, the scan converter's set-up; each change backed by Xenia or libxenon.
    - xsdk sets what holds the rate back the way that doesn't, and HARDWARE.md records what each change did.

    Done when:
    - the probe's fill rate with xsdk's state is near 8 pixels a clock, or HARDWARE.md records each piece tried and
      that none moved it;
    - with what's found reverted on purpose, the probe's rate falls back, on the console;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console, and Dusklight's logo shot draws as before, its GPU
      time measured.

32. **Multisampled targets.** Dusklight has an anti-aliasing setting, and Aurora's vulkan4xenon backend draws with one
    sample whatever it's set to (aurora-xenon:lib/vulkan4xenon/backend.cpp:401-402): xsdk has no multisampling. Xenos
    takes 2 or 4 samples a pixel in eDRAM (`RB_SURFACE_INFO`'s samples, xenia:src/xenia/gpu/registers.h:675;
    xenos.h:288-292; `PA_SC_AA_CONFIG`, `PA_SC_AA_MASK`, register_table.inc:572, 589), and a resolve picks samples or
    averages them (`RB_COPY_CONTROL`'s sample select, registers.h:858; xenos.h:1000-1008), laid out as Xenia's
    draw_util.cc:718-779 has it.

    So:
    - `xsdk_gpu_set_target` and `xsdk_gpu_set_target_at` take 1, 2 or 4 samples; `xsdk_gpu_target_tiles` counts them.
    - Resolves of a multisampled target average its samples into the texture.

    Done when:
    - xsdk_test draws triangles with edges at 1, 2 and 4 samples and resolves each: at 1 the edge pixels are one colour
      or the other, at 2 and 4 they blend in steps of a half and a quarter, every pixel off the edges as at 1;
      broken on purpose, a resolve copying one sample, the check fails on the console;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console.

    vulkan4xenon's multisampled images and Aurora's setting are vulkan4xenon's milestone after this.

33. **Anisotropic filtering.** Aurora asks for 16 times anisotropic filtering by default
    (aurora-xenon:lib/aurora.cpp:75-76), and vulkan4xenon refuses it (vulkan4xenon:src/sampler.c:46-47): xsdk's
    samplers have none. The fetch constant has an anisotropy field (`aniso_filter`, xenia:src/xenia/gpu/xenos.h:140-148,
    1259).

    So:
    - `XsdkSampler` takes the most anisotropy, 1 to 16.

    Done when:
    - xsdk_test draws a texture of thin vertical stripes on a quad squeezed 8 to 1 down the stripes: sampled with
      16 times anisotropy the stripes keep their contrast, and sampled without, the level the GPU picks greys them;
      broken on purpose, the field left out, the check fails on the console;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console.

34. **Fence waits, and drawing after a resolve.** `xsdk_gpu_wait` sleeps a millisecond between looks at the fence
    (src/gpu.c:444-449), so a wait ends up to a millisecond or more after the GPU passes it: a benchmark's every batch
    came out a whole number of milliseconds. vulkan4xenon waits on fences every frame. And a draw after a resolve,
    without setting the target again, hung the GPU: a resolve leaves eDRAM copying, the viewport transform off. The
    header says a resolve undoes `xsdk_gpu_set_target`, not that it undoes `xsdk_gpu_set_target_at` too.

    So:
    - `xsdk_gpu_wait` looks at the fence without sleeping for a short while first, then sleeps as now.
    - A draw after a resolve without the target set again is caught with an `assert`, and the header says so.

    Done when:
    - xsdk_test waits on a fence after a short GPU job and the wait returns within 100 us of the fence passing, as
      the GPU's own time stamps show; broken on purpose, sleeping first, the check fails on the console;
    - the assert holds through every test, and a title that draws after a resolve stops at it with a log line, on the
      console;
    - xsdk_test, vulkan4xenon_test and vkbench pass on the console.

    Looking first slowed Dusklight's frames 5%, its main thread sharing a core with Aurora's GPU waiter, so on
    2026-10-14 waits went back to sleeping between looks unless a title asks to look first
    (`xsdk_gpu_set_wait_spin`); xsdk_test's check asks (HARDWARE.md, "Milestones 30 to 35 together, and fence waits
    that sleep again").

35. **Loop unrolling.** GCC at -O2 keeps a loop of 8 independent multiply-add chains rolled, 8 multiply-adds to a
    branch, and a thread does 3.6 GFLOPS of double multiply-adds through it, 0.56 multiply-adds a clock at 3.2 GHz.
    Whether unrolling gets more out of the in-order core, and costs Dusklight nothing, is to be measured. The
    toolchain's default is what every title and port gets.

    So:
    - Dusklight's title scene and logo shot, and a CPU benchmark, with `-funroll-loops` and without, the GPU and
      CPU times of each part from the diagnostics build.
    - If Dusklight's frame doesn't get slower and its code grows by an amount it can carry, the toolchain's specs
      turn it on for -O2 and above (tools/toolchain).

    Done when:
    - HARDWARE.md records the numbers, on the console;
    - if it's turned on, xsdk_test, vulkan4xenon_test, vkbench and Dusklight pass on the console with the rebuilt
      titles; if not, the record says why.

36. **The texel rate.** Agreed and done on 2026-10-14: docs/scope/36-texel-rate.md (HARDWARE.md, "The texel rate,
    milestone 36").

37. **The Guide.** Agreed and done on 2026-10-15: docs/scope/37-guide.md (HARDWARE.md, "The Guide, milestone 37").

38. **The cost of a draw.** Agreed and done on 2026-10-15: docs/scope/38-draw-cost.md (HARDWARE.md, "The cost of a
    draw, milestone 38").

39. **The GPU's cost of a draw.** Agreed and done on 2026-10-15: docs/scope/39-gpu-draw-cost.md (HARDWARE.md, "The
    GPU's cost of a draw, milestone 39").

**Networking**, a scope of its own rather than a numbered milestone: sockets, and TLS 1.2 in the title's own code.
Agreed and done on 2026-10-14: docs/scope/networking.md.

**Compiling shaders on the console**, a scope of its own: programs a pack doesn't hold compiled the first time a game
asks for them, through glslang and xost's compiler in C, both on the console. Agreed in outline on 2026-10-14:
docs/scope/shader-compiling.md.

**Waiting for a use** (CODE_GUIDELINES.md 1.2), each a milestone when something running on xsdk needs it: render targets
of 64 bits a pixel and resolves into other formats (Xenia's ColorRenderTargetFormat); 128-bit textures
(`k_32_32_32_32_FLOAT`); clearing eDRAM without a copy; memory export from shaders, which xost doesn't emit. The CPU's
atomic increments cost what GCC's sequentially consistent default asks, a full barrier each side; xsdk's own locks are
the kernel's critical sections and its atomics ask only the ordering they need, so there's nothing to change in xsdk.

## 5. Questions for the user first

1. **The GCC:** its version (C++23 needs 13 or later for most of it), target triple, ABI and code model, its C
   library and libstdc++ configuration (threads, TLS model), and whether it produces code the retail kernel's ABI
   accepts (sections 2.1, 2.5).
2. **The console:** RGH or JTAG, its kernel and dashboard version, and which XEX checks it is known to skip.
3. **Deploying and logging:** how they get a `.xex` onto the console, launch it, and read a log today.
4. **The microcode:** their preference among the three options in section 3.2.
5. **xost's licence,** and whether the SDK should share it (xost:docs/license-audit.md:59-87).

## 6. Risks

- **The command processor microcode (section 3.2).** If neither run-time option works, the SDK either ships firmware
  of disputed provenance or can't initialise the GPU itself.
- **The retail `Vd*` setup sequence** is only partly documented; Xenia stubs several calls (section 3.1). Milestones 4
  and 5 discover it empirically, and a wrong sequence may hang the console.
- **What the kernel enforces on a XEX** varies by kernel patch (section 2.2). Milestone 1 settles it for the user's
  console only.
- **Silent GPU hangs.** Bad shaders (the 16-register ceiling) and bad packets hang without errors. Every GPU milestone
  needs the smallest possible step between hardware tests.
- **The ABI details** in section 2.1 are unverified, and a mismatch shows up as crashes in kernel calls.
- **C++ exceptions** depend on libgcc's unwinder working under the kernel's own exception handling, which is
  unverified (section 2.5). If it doesn't, the SDK ships `-fno-exceptions` until it's solved.

## 7. References

All are inside `xenos-toolchain.zip`.

**Xenia** (BSD-3), `reference/xenia`, commit `95a5c3e`:
- `src/xenia/kernel/util/xex2_info.h`: XEX2 headers, optional header keys, security info, import libraries, TLS.
- `src/xenia/cpu/xex_module.cc`: PE checks inside the XEX, import records and thunks.
- `third_party/pe/pe_image.h`: PE constants (subsystem, machine).
- `src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc`, `src/xenia/kernel/xam/xam_table.inc`: export ordinals.
- `src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc`, `xboxkrnl_memory.cc`, `xboxkrnl_audio.cc`: the kernel's video,
  memory and audio exports.
- `src/xenia/gpu/graphics_system.cc`: the GPU register window and `CP_RB_WPTR`.
- `src/xenia/gpu/xenos.h`: PM4 opcodes, packet builders, fetch constants, texture formats.
- `src/xenia/gpu/register_table.inc`: register numbers.
- `src/xenia/gpu/command_processor.cc`: packet semantics, including `IM_LOAD`.
- `src/xenia/gpu/draw_util.h`: resolves. `src/xenia/gpu/texture_address.h`: tiling. `src/xenia/gpu/ucode.h`: the
  shader ISA.

**libxenon** (BSD-3), `reference/libxenon/libxenon`:
- `drivers/xenos/xe.c`: bare-metal CP setup and microcode load, and the shader container loader.
- `drivers/xenos/ucode.c`: the embedded CP microcode.

**libxenon's toolchain script:** `reference/libxenon/toolchain/build-xenon-toolchain`.

**xost** (the user's): `README.md`, `ROADMAP.md`, `docs/xsc.md`, `docs/container.md`, `docs/HANDOFF.md`,
`docs/license-audit.md`.
