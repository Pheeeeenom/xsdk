# Hardware results

Facts the user's RGH console has settled. Each entry says what ran, what happened, and when.

## XEX loading (2026-10-09)

Launched from Aurora, logs read through xbWatson and `xsdk.log`.

- **Accepted:** an unsigned, unencrypted XEX2 in retail format with BASIC compression (one raw block), written by
  `elf2xex`. A copy re-packed by XexTool as devkit, encrypted and uncompressed also runs.
- **Refused:** every earlier build. Their import thunks had zero in word 1. The working builds differ in four ways at
  once, so which one the loader needed isn't isolated:
  - thunks hold `01LLOOOO 02LLOOOO mtctr r11 bctr` and are 16-byte aligned;
  - import slots are in read-only data;
  - the imports claim 2.0.21256.0 with minimum 2.0.1861.0 (it was 2.0.17559.0 for both);
  - there's no default heap size header.
- **The working layout** matches an XDK-built homebrew title that runs on the same console.
  - Its import library IDs are `0xFCA15C76` for xam.xex and `0x45DC17E0` for xboxkrnl.exe.
  - It has one page descriptor per 64 KB page.
  - Its import libraries header ends the XEX headers.

## XEX checks the kernel enforces (2026-10-09)

`tests/xex_checks.py` copies the working title with one digest broken by a single flipped byte:

| Broken digest | Result |
|---|---|
| Header digest (security info +0x164) | Launches and passes every test, so not enforced |
| Page digest (a byte nothing reads, in page 0) | Launches and passes every test, so not enforced |
| Import table digest (security info +0x12C) | Refused, so enforced |

An xbWatson minidump's system information reports build 21256, but XAM's D3D hang report on the same console says
kernel 17559 (both 2026-10-10), so which number is the kernel's isn't settled. The minidump's module list names the
kernel `xboxkrnld.exe` and XAM `xamd.dll`, with `Xbdm.xex` loaded. The patch set isn't recorded; another patch set may
differ.

## Runtime (2026-10-09)

The test title (`tests/main.c`) ran and logged:

- `game:` with root `OB_DOS_DEVICES` resolves to the folder the title was launched from, and it's writable.
- Read-only data, initialised data and zeroed data are loaded and laid out as linked. Data is writable.
- Kernel calls from GCC's 32-bit code work through `tools/gen_imports.py` stubs:
  - `NtCreateFile`, which takes 9 arguments, so arguments past the eighth reach the kernel from `r1+0x54`;
  - `NtWriteFile`;
  - `NtClose`;
  - `NtAllocateVirtualMemory`, which newlib's `malloc` reaches through `sbrk`;
  - the variadic `DbgPrint`.
- newlib's printf formats correctly. The two stray NUL bytes per conversion seen in Xenia come from Xenia skipping
  `mcrf`.
- `XamLoaderTerminateTitle` returns to the dashboard and doesn't come back.

## Runtime, milestone 2 (2026-10-09)

The test title passed all 12 checks on the console:

- **Threads.** `ExCreateThread` with an XAPI startup routine calls it with the start address and context, as in
  Xenia (xthread.cc:518-521). `ExTerminateThread` ends the thread, and `NtWaitForSingleObjectEx` on its handle
  returns.
- **Synchronisation.** Two semaphores and a synchronization event hand 100 items from one thread to another (sum 5050).
  Two threads log through the critical-section-guarded log without corrupting it.
- **Files through newlib.** `fopen`, `fputs`, `fgets`, `fseek`, `ftell` and append mode work on
  `game:\xsdk_file_test.txt` through the SDK's `open`, `read`, `write`, `lseek` and `fstat`.
- **Controller.** `XamInputGetState(0, 0, &state)` returns live state:
  - the packet number advances only when the state changes;
  - sticks are signed 16-bit over the full range;
  - triggers run 0-255;
  - buttons are a bit mask, for example `0x0200` and `0xA200`.
- **Audio.**
  - The render driver calls the client callback 376 times in 2000 ms. That's one call per 256-sample frame at
    48 kHz, matching Xenia's frame size and rate.
  - The callback may call `XAudioSubmitRenderDriverFrame` itself.
  - `MmAllocatePhysicalMemoryEx` returns usable memory for the frame.
  - The frame layout plays as intended. Checked by ear on a stereo TV with `audio_check.xex`:
    - channel 0 sounds from the left speaker only, channel 1 from the right only;
    - the centre and back channels sound as a stereo downmix allows;
    - a 750 Hz tone sounds an octave above 375 Hz.
    So frames are 6 channels of big-endian floats, channel after channel, in Xenia's 5.1 order (front left, front
    right, centre, LFE, back left, back right).

## C++23 runtime, milestone 3 (2026-10-10)

The test title, built with the GCC 15 toolchain, passed all 30 checks on the console; the log is in
`results/2026-10-10-milestone-3/xsdk.log`.

- **Start-up.** A static constructor in `.init_array` runs before `main`. 64-byte, 16-byte and page-aligned
  allocations come back aligned.
- **Threads.** Two `std::jthread`s rally 100 times each through a `std::mutex` and a `std::condition_variable`, and see
  their own `thread_local` values (emulated TLS on `KeTlsAlloc`/`KeTlsGetValue`/`KeTlsSetValue`).
- **Atomics.** Four threads counting 10,000 times each give exact 32-bit and 64-bit totals, so the inline
  `lwarx`/`stwcx.` and `ldarx`/`stdcx.` loops work. Over a 200 ms `std::atomic::wait` the waiter yielded 16 times and
  blocked once, so it sleeps.
- **Time.** A 250 ms `sleep_for` measured 249 ms and 250 ms on `steady_clock` in two runs, so
  `KeQueryPerformanceFrequency` gives the time base's rate to within that. `system_clock` reads the right date from
  `KeQuerySystemTime`.
- **File system.** `std::filesystem` creates, lists and removes a directory under `game:`:
  - `NtCreateFile` with `FILE_CREATE` and `FILE_DIRECTORY_FILE` creates it;
  - `NtQueryDirectoryFile`, given the pattern `*` on the first call and none after, lists it;
  - `NtSetInformationFile` with `FileDispositionInformation`, then `NtClose`, deletes the file and then the empty
    directory. Xenia ignores this, so the console is the first proof.
- **Formatting.** `std::format`, `std::expected` and `std::println` work; `println` reaches the log through stdout.
- **Exceptions.** A throw is caught, an `exception_ptr` from another `std::jthread` rethrows, and `dynamic_cast` and
  `typeid` work. Before the unwinder register-size fix in `tools/toolchain/gcc-xsdk.patch`, the first throw ended in
  an access violation at a stack address, 0x7010FA4C, repeated as first-chance exceptions in xbWatson.

The rule that every `lwarx` pairs with a `stwcx.` (`tools/toolchain/gcc-xsdk.patch`) isn't observable in this run and
stays unverified.

## Command processor at title start (2026-10-10)

`tests/probe_cp.c` read the GPU registers from a title launched from Aurora, without halting the command processor
(log: `results/2026-10-10-probe-cp/xsdk.log`). Register meanings come from libxenon:drivers/xenos/xe.c.

- **Titles can read the GPU registers** at 0x7FC80000 under this kernel.
- **The command processor is running and idle at title start.** 0x07D8 is 0x00007FFF, so bit 28 (halted) is clear,
  and the busy bit of 0x1740 is clear. The ring registers describe XAM's ring, as a later run showed: base
  0x01B9D000, control 0x00020309, read pointer write-back at 0x01B9F03C, read and write pointers both 0xEB.
- **The ME microcode reads back** through 0x07E4 and 0x07E8 while the command processor runs: 1100 of 2304 words are
  non-zero, and two reads give the same CRC-32, 7A8E8248. libxenon's copy differs (1178 non-zero words, CRC-32
  4D1AD8C0), so the console's microcode is a different version.
- **Reading the PFP microcode while it runs hangs the GPU.** Two reads through 0x117C and 0x1180 disagree, with 250 of
  288 words non-zero (libxenon's PFP has every word non-zero). Afterwards 0x1740's busy bit was set, 0x07D8 had gained
  bit 29, and once the title exited, debug XAM broke into the debugger with "The GPU is hung and can't be recovered
  without doing a cold boot."

A second run halted the command processor first, as libxenon does: it set bit 28 of 0x07D8 and waited 2 ms before
reading, then wrote back the original value (log: `results/2026-10-10-probe-cp-halted/xsdk.log`).

- **Halting works.** 0x07D8 then reads 0x10007FFF, and the busy bit stays clear.
- **The PFP microcode can't be read back, halted or not.** Two reads through 0x117C and 0x1180 disagreed again, with
  250 non-zero words, so a read of 0x1180 doesn't return the microcode.
- **The ME microcode read the same as in the first run:** 1100 non-zero words, CRC-32 7A8E8248.
- **Touching the PFP microcode RAM wrecks the command processor either way.** After resuming, 0x07D8 read back as
  0x22... (the log ends mid-line), the GPU hung, and XAM again asked for a cold boot.

So SCOPE.md section 3.2's option 1 fails: there's no way to read back the PFP microcode that `VdInitializeEngines`
needs. Reading the ME RAM while the command processor runs turned out to hang the GPU too (see "XAM's D3D device
shares the command processor"), so the probe no longer reads either.

## A ring without VdInitializeEngines (2026-10-10)

`tests/probe_ring.c` skipped `VdInitializeEngines` and used the command processor as it runs under a title, with its
microcode (log: `results/2026-10-10-probe-ring/xsdk.log`).

- **The kernel's ring set-up works on the running command processor.** After `VdInitializeRingBuffer(physical, 12)`
  and `VdEnableRingBufferRPtrWriteBack(physical, 6)`, 0x0700 holds the ring's physical address, 0x0704 reads 0x0002060C
  (size 12 in the low bits, block size 6 from bit 8), and 0x070C holds the write-back address with 2 ORed in.
- **`VdInitializeRingBuffer` leaves both ring pointers at 0xEB, not 0,** with the ring zeroed beforehand. The kernel
  appears to submit 235 dwords of its own; a title must append after the write pointer it finds.
- **The command processor runs a title's packets.** The probe wrote its packets at offset 0 and set the write pointer to
  9, so the command processor wrapped through the zeroed ring first. Even so, the `MEM_WRITE` landed, and with
  address bits 1-0 set to 2 the CPU reads the value as written. The read-pointer write-back reached 9.
- **The `EVENT_WRITE_SHD` fence didn't land** in that run, after about 8,000 zero dwords had each written GPU register 0.
  The GPU ended busy, 0x07D8 gained bit 29, and XAM reported a hung GPU after the title exited.

A second run, after a cold boot, appended its packets at the write pointer the kernel left (log:
`results/2026-10-10-probe-ring-2/xsdk.log`).

- **The pointers moved again during set-up,** from 0xEB to 0x115 (the first run went from 0xC7 to 0xEB), and this time the
  GPU was already busy right after `VdEnableRingBufferRPtrWriteBack`, with 0x07D8 bit 29 set.
- **The CPU read back zeros for every dword before the new write pointer.** The probe had zeroed the write-combined
  ring and called `VdInitializeRingBuffer` without a `sync`, so the zeroing may have landed on top of packets the
  kernel wrote, which would explain both runs.
- **Neither fence landed,** though the read pointer reached the end of the probe's packets.
- **XAM's D3D printed a GPU hang report** after the title exited. It names 0x1740 `RBBM_STATUS` (0x80050110) and the
  ring registers `CP_RB_BASE` (the probe's ring), `CP_RB_RPTR` and `CP_RB_WPTR` (both 0x11E). Its CP debug words held
  the `EVENT_WRITE_SHD` packet's value and address, so the command processor was stuck in that fence.

A third run synced after zeroing the ring, before `VdInitializeRingBuffer`, and submitted libxenon's scratch fence with
a `MEM_WRITE` before the `EVENT_WRITE_SHD` (log: `results/2026-10-10-probe-ring-3/xsdk.log`).

- **The kernel doesn't put packets in the title's ring.** Every dword before the write pointer it left (229, up from
  0xC7) read back as zero.
- **The command processor stalls during the kernel's set-up.** The GPU was busy right after
  `VdEnableRingBufferRPtrWriteBack`, with 0x07D8 bit 29 set. None of the fences landed (`MEM_WRITE`, scratch write-back,
  `EVENT_WRITE_SHD`), though the read pointer followed the write pointer each time. D3D's hang report again showed the
  `EVENT_WRITE_SHD` packet in the command processor's debug words.

So without `VdInitializeEngines` the kernel's ring set-up stalls the command processor (two runs of three; in the
first, one `MEM_WRITE` landed). The probe now sets the ring up through the registers as libxenon does.

A fourth run set the ring up through the registers, as libxenon does, and didn't call the kernel's ring functions (log:
`results/2026-10-10-probe-ring-4/xsdk.log`, from xbWatson's copy).

- **Register set-up ends the same way as the kernel's.** Afterwards 0x0700, 0x0704 and 0x070C held the probe's values,
  but the read and write pointers were both 0xEB, though the probe had written 0 to the write pointer during the
  reset (0xD3 at start). The GPU was busy and 0x07D8 had bit 29 set. So the stall doesn't come from the kernel's ring
  functions. In all four runs something other than the title moved the write pointer during set-up, by 24 to 42
  dwords.
- **Phase A didn't land.** The read pointer write-back reached 13 at the new address, but neither the `MEM_WRITE` nor
  the scratch write-back landed within 1000 ms.
- **The probe crashed on its next packet, through its own fault.** GCC merged two 32-bit ring stores into one `std` at
  0xBFC40034, which isn't 8-byte aligned, and that raised an alignment exception in write-combined memory (xbWatson:
  EADDR 0x82010A04, `std r10,52(r28)`, r10 = 0xC0025800_00000014, the packet's header and event). Code that writes
  write-combined memory the GPU reads must use volatile 32-bit stores. The title carried on and submitted phase B.
- **XAM's D3D printed hang reports after the title exited.** The first had the probe's ring at both pointers 0x11 and
  the `EVENT_WRITE_SHD` packet's value and address in the CP debug words. The later ones (frames 18 to 20) had
  `CP_RB_BASE` 0, `CP_RB_RPTR` 0 and `CP_RB_WPTR` climbing from 0xFD to 0x11B, so that device kept submitting to a
  command processor that no longer fetched.

What moved the write pointer during set-up was XAM's D3D device, which keeps submitting while a title runs (next
section).

## XAM's D3D device shares the command processor (2026-10-10)

`tests/probe_cp.c` watched the ring registers for five seconds without writing to them, sampling about every 2 ms (log:
`results/2026-10-10-probe-cp-watch/xsdk.log`).

- **XAM holds a D3D device while a title runs, and the title has none.** `VdGlobalXamDevice` holds 0x7882FA00 and
  `VdGlobalDevice` holds 0. 0x7882FA00 is the device every GPU hang report in these runs names. Its non-snooped
  block, 0xE1B9D000, matches the ring base titles find, 0x01B9D000.
- **XAM submits to that ring every vblank.** The read and write pointers moved together by 12 dwords every 16 to 18 ms,
  wrapping at 0x400 (the control value's size, 9, is 2^9 8-byte units). The GPU was busy in 13 of 2507 samples, each
  time with 0x07D8 bit 29 set, so bit 29 comes with ordinary work and doesn't by itself mean a stall.
- **This explains the ring runs.** Once a title points 0x0700 at its own ring, XAM's next vblank submission writes
  XAM's write pointer to 0x0714. The command processor then runs whatever that index covers in the title's ring, and
  XAM's fences never complete. The 24 to 42 dwords the write pointer gained during set-up are two or three of those
  submissions.
- **XAM checks its device for hangs on its own thread.** In an xbWatson minidump of the break that follows each hang
  report, the breaking thread was created by XAM (start 0x816B9668), every frame is in `xamd.dll` or the kernel, r29
  and r31 hold 0x7882FA00, and r3 points at "The GPU is hung and can't be recovered without doing a cold boot."
- **Reading the ME microcode while the command processor runs hangs the GPU.** The GPU was healthy through the watch
  and afterwards, then the probe read the ME RAM through 0x07E4 and 0x07E8 (CRC-32 7A8E8248 again) and exited, and
  XAM broke on a hung GPU. That read was the probe's only write to the GPU.

So a title that wants the command processor has to stop XAM's device from submitting first. Xenia keeps a single
graphics interrupt callback that `VdSetGraphicsInterruptCallback` replaces and that vblank calls with source 0
(xenia:src/xenia/gpu/graphics_system.cc:234-272), so if XAM's vblank submissions run from that callback, a title that
sets its own stops them.

## A title's interrupt callback stops XAM's device (2026-10-10)

`tests/probe_interrupt.c` watched XAM's write pointer for 2 seconds at a time, without writing GPU registers (log:
`results/2026-10-10-probe-interrupt/xsdk.log`).

- **XAM submits from its graphics interrupt callback.** With XAM's callback in place the write pointer moved 121 times
  in 2000 ms (0xDF to 0x27F). After `VdSetGraphicsInterruptCallback` set the title's callback it didn't move at all,
  and it stayed at 0x28B after the title cleared the callback with `VdSetGraphicsInterruptCallback(0, 0)`. Nothing put
  XAM's callback back while the title ran.
- **The callback's vblank source fires twice per frame.** The title's callback ran 241 times in 2000 ms, every time
  with source 0, twice the rate XAM submitted at. Xenia calls it once per vsync (120 times in 2000 ms,
  xenia:src/xenia/gpu/graphics_system.cc:272).
- **The GPU stayed idle,** and the ring registers kept XAM's ring.
- **Taking XAM's callback breaks XAM once the title exits.** The console froze on the way back to Aurora. In an
  xbWatson minidump taken then, XAM's thread that checks its device holds "The GPU is hung and can't be recovered
  without doing a cold boot." in its live frames. Aurora's main thread is blocked early in its start-up: Aurora called
  into the kernel (return address 0x800C7E4C), the kernel called into XAM (`xamd.dll`+0xDC278), and XAM is waiting.
  The probe never wrote a GPU register and left the GPU idle, so XAM's device seems to need its callback to finish its
  work, and nothing gave it back. A title can't give it back itself, since no export returns the current callback.

So a title's callback stops XAM's device, but XAM can't recover afterwards. Aurora's start-up waits on XAM through the
kernel, which suggests D3D titles take the GPU from XAM through a kernel call. Xenia lists exports that fit
(`VdRegisterXamGraphicsNotification`, `VdCallGraphicsNotificationRoutines`, `VdBlockUntilGUIIdle`) but stubs them
(xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc:446-447, 476; xboxkrnl_video.cc:310-319).

## Aurora takes the GPU from XAM in VdInitializeEngines (2026-10-10)

`tests/probe_exports.c` asked the kernel for every export's address with `XexGetProcedureAddress` (921 of 1023
ordinals resolve) and placed the kernel return addresses of Aurora's blocked start-up among them (log:
`results/2026-10-10-probe-exports/xsdk.log`).

- **0x800C7E4C is in `VdInitializeEngines`:** 0x34 past its start at 0x800C7E18, and below `VdShutdownEngines` at
  0x800C7F60.
- **0x800BD7E8 is in `VdCallGraphicsNotificationRoutines`:** 0x40 past its start at 0x800BD7A8, and below
  `VdRegisterGraphicsNotification` at 0x800BD840.
- **0x800C7DFC is in an unexported function** just below `VdInitializeEngines`.

So Aurora's D3D called `VdInitializeEngines`, which ran the graphics notification routines, and the one in `xamd.dll`
waited for XAM's device to stop. That device couldn't stop without its callback. A title takes the GPU from XAM in
the same call that loads the microcode.

## VdInitializeEngines without microcode (2026-10-10)

`tests/probe_engines.c` called `VdInitializeEngines(0x4F810000, callback, 0, 0, 0)`, with no microcode, and left XAM's
interrupt callback alone (log: `results/2026-10-10-probe-engines/xsdk.log`).

- **The kernel takes the GPU from XAM without microcode.** The call returned 1 and never called its callback.
  Afterwards `VdGlobalXamDevice` held 0, XAM's write pointer stopped moving, the ring registers were cleared (base 0,
  control 0x08020000, both pointers 0), the GPU was idle, and 0x07D8 read 0x06007FFF (bits 25 and 26 newly set).
- **The kernel's ring set-up then works.** After `VdInitializeRingBuffer` and `VdEnableRingBufferRPtrWriteBack`,
  0x0700 and 0x070C held the probe's addresses, 0x0704 read 0x0002060C, both pointers were 0, and the GPU was idle.
- **The command processor runs the title's packets without the title supplying microcode.** The `MEM_WRITE` and
  libxenon's scratch fence both landed within 1 ms, and the read pointer write-back reached 13. Whether the kernel kept
  the running microcode or loaded its own doesn't show.
- **`EVENT_WRITE_SHD` with `CACHE_FLUSH_AND_INV_TS_EVENT` (20) still wedges the GPU.** The read pointer reached the
  packet's end, the value never landed, the GPU went busy and 0x07D8 gained bit 29. Xenia describes the packet as
  generating "a VS|PS_done event" (xenia:src/xenia/gpu/xenos.h:1595), so event 20, a cache flush, may be the wrong
  kind for it, or it may need pipeline state nothing has set yet.
- **`VdShutdownEngines` didn't return,** presumably held up by the wedged GPU, and the console stopped responding.
  xbWatson reported no exception.

So SCOPE.md section 3.2 has an answer in option 2's spirit: a title takes the command processor with
`VdInitializeEngines` and no microcode, and the kernel's ring calls work after it. Still open are giving the GPU back
cleanly and an `EVENT_WRITE` fence.

## Giving the GPU back with VdShutdownEngines (2026-10-10)

`tests/probe_engines.c` took the GPU as before, submitted three scratch fences, and gave the GPU back with
`VdShutdownEngines` (log: `results/2026-10-10-probe-engines-2/xsdk.log`, xbWatson's copy; the file on the console
stopped mid-line at 2050 bytes).

- **Fences repeat.** Each of the three fences' `MEM_WRITE` and scratch values landed within 1 ms, and the read pointer
  write-back reached 0x0D, 0x16 and 0x1F.
- **`VdShutdownEngines` gives the GPU back to XAM.** Afterwards 0x07D8 read 0x00007FFF again, the ring registers held
  XAM's ring again (base 0x01B9D000, control 0x00020309, write-back 0x01B9F03E), `VdGlobalXamDevice` held 0x7882FA00
  again, and XAM's write pointer moved 15 times in 250 ms.
- **`VdShutdownEngines` calls the callback passed to `VdInitializeEngines`,** once. The probe's callback takes no
  arguments.
- **The title exited normally and the console returned to Aurora.**

So a title drives the command processor with `VdInitializeEngines(0x4F810000, callback, context, 0, 0)`,
`VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`, its packets, and `VdShutdownEngines` before it exits.

## An EVENT_WRITE fence (2026-10-10)

`tests/probe_engines.c` took the GPU and ran the three scratch fences as before, then tried `EVENT_WRITE_SHD` with
three events, stopping at the first that didn't land (log: `results/2026-10-10-probe-engines-3/xsdk.log`, xbWatson's
copy).

- **`EVENT_WRITE_SHD` with a shader-done event is a working fence.** With `VS_DONE_TS` (2) and with `PS_DONE_TS` (3)
  the value landed within 1 ms, and the read pointer write-back reached 0x23 and 0x27.
- **With `CACHE_FLUSH_TS` (4) it wedged the GPU,** like `CACHE_FLUSH_AND_INV_TS_EVENT` (20) before: the read pointer
  reached the end of the packet, the value never landed, the GPU stayed busy and 0x07D8 gained bit 29. The cache-flush
  events presumably need state for the caches they flush, which nothing had set up.
- **The probe then exited without `VdShutdownEngines`,** which would have waited on the wedged GPU.

So milestone 4's fence is `EVENT_WRITE_SHD` with `PS_DONE_TS` or `VS_DONE_TS`. The cache-flush events wait for the
render state of milestone 5.

## The command processor, milestone 4 (2026-10-10)

The test title passed all 34 checks on the console, the four `gpu:` checks among them, exited with status 0, and the
console returned to Aurora; the log is in `results/2026-10-10-milestone-4/xsdk.log`.

- **`xsdk_gpu_open` takes the GPU** with `VdInitializeEngines` and no microcode, and sets up the ring and the read
  pointer write-back with the kernel's calls.
- **A `NOP` and an `EVENT_WRITE_SHD` fence with `PS_DONE_TS` land.**
- **The ring wraps.** About 4,000 `NOP`s of 2 to 5 dwords, each followed by a fence, about 30,000 dwords in all, wrapped
  the 8192-dword ring nearly four times, with a `NOP` filling each wrap, and the last fence landed.
- **`xsdk_gpu_close` gives the GPU back.** It waited for a final fence, called `VdShutdownEngines`, and freed the ring
  and the write-back page with `MmFreePhysicalMemory(0, base)`, and the title then exited normally.

So milestone 4 is done, and SCOPE.md section 3.2 has its answer.

## VdSwap writes only a fence (2026-10-10)

`tests/probe_swap.c` took the GPU, filled a linear 1280x720 `k_8_8_8_8` front buffer from the CPU, and called `VdSwap`
on it with the arguments Xenia documents (log: `results/2026-10-10-probe-swap/xsdk.log`).

- **The display information differs from Xenia's.** `VdQueryVideoMode` gave 1280x720 at 59.94 Hz, hi-def and
  widescreen, but `VdGetCurrentDisplayInformation` gave a 640x480 front buffer for a 1280x720 display.
- **`VdGetSystemCommandBuffer` zeroes 0x94 bytes** of its first argument, as Xenia does, and wrote 0x471D to its second.
- **`VdSwap` wrote one fence into the 64 reserved dwords:** `EVENT_WRITE_SHD` with `PS_DONE_TS`, writing the fifth
  argument to the third (the "system writeback", with byte swap 2), then a `NOP` over the other 59. The fence landed.
  There were no display registers or front buffer address in the ring, so the flip happens outside it.
- **The screen stayed black** through the whole run.

## The log file keeps its old size (2026-10-10)

Two logs came back cut off mid-line, each at the length of the run before it: the second engines run's at 2050 bytes,
the first engines run's length, and the second swap run's at 1454 bytes, the first swap run's length. So when the
title rewrites `xsdk.log` with `FILE_OVERWRITE_IF`, the new text reaches the disk but the file's size stays the old
one, even after `NtClose` in `_exit`, and a longer log loses its tail. The log now calls `NtFlushBuffersFile` after
every line; whether that keeps the size current is for the next run to show.

## The kernel doesn't flip on VdSwap (2026-10-10)

`tests/probe_swap.c` registered the swap fence's address with `VdSetSystemCommandBufferGpuIdentifierAddress`, passed
the value from `VdGetSystemCommandBuffer` as `VdSwap`'s fifth argument, and swapped first to a red 1280x720 buffer,
then to a blue 640x480 one, reading the display controller's registers (libxenon:drivers/xenos/xenos.h:26-44) along
the way (log: `results/2026-10-10-probe-swap-3/xsdk.log`).

- **At title start the display shows a 640x480 surface at address 0, scaled up.** `D1GRPH_ENABLE` 1,
  `D1GRPH_CONTROL` 2, `D1GRPH_PRIMARY_SURFACE_ADDRESS` 0, pitch and end 640x480, viewport size 640x480, scaler on.
  That matches the 640x480 front buffer `VdGetCurrentDisplayInformation` reports.
- **Neither swap changed the display.** Each `VdSwap` wrote the same fence, now with the value from
  `VdGetSystemCommandBuffer`, and it landed, but the surface address stayed 0 and the display information stayed
  640x480, so the screen stayed black. So the kernel doesn't flip on `VdSwap` and its fence alone.
