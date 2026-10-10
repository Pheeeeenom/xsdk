# Xbox 360 SDK: scope of the SDK layer and the Xenos GPU layer

**Goal.** A from-scratch Xbox 360 homebrew SDK that builds `.xex` executables that the retail kernel and dashboard
launch, on a modded console (RGH/JTAG). On top of it comes a GPU layer that drives Xenos directly. A translation layer
like vulkan4vita comes later and is out of this scope. So are games.

**Constraints.**
- No Microsoft tooling and no leaked SDK material.
- No OpenXeChain.
- Public reverse-engineering, mainly the Xenia emulator's source, is the reference, and every fact below cites it.

**Sources.**
- **Xenia.** Xenia's own copy is in `xenos-toolchain.zip` under `xenos-toolchain/reference/xenia` (commit `95a5c3e`).
  It's cited below as `xenia:<path>:<line>`, paths relative to `reference/xenia/`.
- **libxenon.** Its copy is in the same zip, cited as `libxenon:<path>:<line>`, relative to
  `reference/libxenon/libxenon/`.
- **The user's toolchain** is cited as `xost:<path>:<line>`, relative to `xenos-toolchain/`.
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
  mutex and condition variable elsewhere. The SDK gets the latter through gthreads. On the Vita that path spun and
  yielded before sleeping, which cost real CPU (vulkan4vita milestone 25). Test that it sleeps promptly.
- **Exceptions and RTTI.** RTTI needs only the compiler. Exceptions need libgcc's DWARF unwinder, and that needs:
  - `elf2xex` keeping `.eh_frame` and `.gcc_except_table`;
  - `crt0` registering the frames with `__register_frame_info`, since there's no `dl_iterate_phdr` without an ELF
    loader.

  Whether the kernel's own exception dispatch gets in the way of libgcc's unwinding is unverified. The test title
  checks a throw across threads and across a kernel callback.
- **`std::chrono`.**
  - `system_clock` comes from `KeQuerySystemTime` (0x084), NT time: 100 ns since 1601.
  - `steady_clock` comes from the CPU's time base (`mftb`), scaled by its frequency. The frequency is unverified:
    read it from `KeQueryPerformanceFrequency` (0x083) at start-up rather than hard-coding it.
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
- **Recommendation:** a host-side script that uploads, launches and fetches the log in one step, as the Vita project
  does through FTP and a command port.

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
   - Xenia fills the reserved region itself (xboxkrnl_video.cc:405-420). What the real kernel writes there is
     unverified, so the SDK should reserve it exactly as a title does and let the kernel fill it.
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
- **Presenting.** The front buffer `VdSwap` shows is such a resolve target in memory.
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
   and presents it with `VdSwap`, every frame, in a colour that changes over time.
6. **A triangle.** Done when a xost vertex and pixel shader, loaded by the SDK's own loader with `IM_LOAD`, draw a
   coloured triangle from a vertex buffer, with the vertex format set by fetch constants.
7. **A textured quad.** Done when a texture uploaded by the SDK samples correctly: linear first, then tiled through the
   SDK's `Tiled2D`.
8. **Render to texture.** Done when a scene resolves to a texture that a second pass samples. A translation layer
   relies on exactly this.
9. **Frame pacing.** Done when frames are paced to vsync through the interrupt callback and fences, with two frames in
   flight and no stalls or tearing over a 10-minute run.

After milestone 9, the SDK and the GPU layer are a complete base. A later translation layer can build on them the way
vulkan4vita builds on GXM: render passes on eDRAM and resolves, descriptors on fetch and shader constants, and shaders
from xost's GLSL path. Scoping that layer is a separate document.

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
