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
- **The `EVENT_WRITE_SHD` fence didn't land** in that run, after about 8,000 zero dwords had each written GPU register
  0. The GPU ended busy, 0x07D8 gained bit 29, and XAM reported a hung GPU after the title exited.

A second run, after a cold boot, appended its packets at the write pointer the kernel left (log:
`results/2026-10-10-probe-ring-2/xsdk.log`).

- **The pointers moved again during set-up,** from 0xEB to 0x115 (the first run went from 0xC7 to 0xEB), and this time
  the GPU was already busy right after `VdEnableRingBufferRPtrWriteBack`, with 0x07D8 bit 29 set.
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

## Logs cut off at the previous run's length (2026-10-10)

Four logs came back cut off mid-line, each at exactly the length of the run before it: 2050 bytes after a 2050-byte
log, 1454 after 1454, 962 after 962, and 1002 after 1002. Calling `NtFlushBuffersFile` after every line didn't change
that, so it was reverted. A shorter log always came back whole, which fits the FTP transfer reusing the old file's size
rather than the console's file system; that's unconfirmed. xbWatson's copy of a log is complete.

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

## The display takes a title's surface but stays black (2026-10-10)

`tests/probe_display.c` kept the kernel's 640x480 mode and wrote only `D1GRPH_PRIMARY_SURFACE_ADDRESS`, inside
libxenon's update lock (`D1GRPH_UPDATE` 1, then 0; libxenon:drivers/xenos/xenos.c:366-379), pointing it at a buffer the
CPU filled (log: `results/2026-10-10-probe-display/xsdk.log`).

- **The display controller takes the address.** The register read back as the buffer's physical address, 0x1FB00000,
  with `D1GRPH_UPDATE` reading 4 just after the write and 0 by the end, and the old address went back the same way.
- **The screen stayed black** through red, green and blue in libxenon's pixel layout and the tiled stripe pattern. So
  something after the surface keeps the output black while a title runs.

## VdSetDisplayMode doesn't change the display (2026-10-10)

`tests/probe_display.c` called `VdSetDisplayMode(0x40000000)` after taking the GPU, then swapped with `VdSwap`, then set
the surface directly (log: `results/2026-10-10-probe-display-2/xsdk.log`, xbWatson's copy).

- **`VdSetDisplayMode` returned 0 and changed none of the display registers** or the 640x480 display information.
- **`VdSwap` again only landed its fence;** setting the surface directly again took, and the screen stayed black.
- **The LUT looks ordinary:** 0x64C0 to 0x64DC read 0, 0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0, black offsets 0 and white
  offsets at the maximum if they follow libxenon's `DC_LUTA_CONTROL` at 0x64C0. 0x6080 to 0x60BC, a range libxenon
  doesn't use, read all zeros.

## How XAM switches the display (2026-10-10)

`tests/probe_guide.c` left the GPU with XAM and logged every change to the display registers around libxenon's names
for 15 seconds while the user opened and closed the Guide (log: `results/2026-10-10-probe-guide/xsdk.log`).

- **XAM switched the display to its own surface twice and back,** at 1021 and 6701 ms, back at 4890 and 9654 ms. Each
  time it changed only these registers, all of them back afterwards:
  - 0x6110 `D1GRPH_PRIMARY_SURFACE_ADDRESS`: 0 to 0x01C10000;
  - 0x6120 `D1GRPH_PITCH` and 0x6134 `D1GRPH_X_END`: 640 to 864;
  - 0x6584 `AVIVO_D1MODE_VIEWPORT_SIZE`: 640x480 to 852x480;
  - 0x65B4: 0x00800000 to 0x00AA6660, the ratio 852/640 if 0x00800000 is 1.0, and 0x65B8: 0x0005C000 to 0x0005D600;
  - 0x65E8: 0x9F to 0xD4, the viewport width / 4 - 1, as libxenon writes it (libxenon:drivers/xenos/xenos.c:390);
  - 0x60EC, libxenon's `D1CRTC_DOUBLE_BUFFER_CONTROL`: 1 in the sample where the new values appeared, 0 by the next.
- **Nothing else changed:** no blanking or LUT register. Several registers count or toggle every frame (0x6024, 0x6054,
  0x6058 to 0x6068, 0x6530, 0x6554, 0x6578, 0x657C), and 0x6144 `D1GRPH_UPDATE` read 2 for a sample every so often,
  with 0x6534 and 0x6544 going to 0x1000 and 0x10 at the same moments.

So XAM shows its surface the way the probes tried, plus 0x60EC, which the probes never wrote.

## XAM's display registers don't show a title's buffer either (2026-10-10)

`tests/probe_display.c` took the GPU and applied a surface the way XAM did: the registers, then 0x60EC = 1. First only
the address of a red buffer in the kernel's 640x480 mode, then XAM's whole set (852x480 at pitch 864, its scaler
values) on a blue buffer, then the originals (log: `results/2026-10-10-probe-display-3/xsdk.log`).

- **Every register took its value, and 0x60EC read 0 again straight away** each time, where XAM's read 1 for a sample.
- **The screen stayed black** throughout. Every colour the probes have shown had a zero low byte, which is the alpha if
  the display reads pixels little-endian, as libxenon's console suggests (libxenon:drivers/console/console.c:67-70).

The Guide did appear on screen during the run that recorded XAM's registers, so the output isn't muted while a title
runs: XAM's surface shows through the same registers that leave the title's buffers black.

## A title can't move the display while XAM has the GPU (2026-10-10)

`tests/probe_display.c` showed opaque white (0xFFFFFFFF) from a buffer high in memory (physical 0x1FB20000) and one low
(0x03ED0000), both before taking the GPU, then the high one after taking it, then red with an opaque alpha (log:
`results/2026-10-10-probe-display-4/xsdk.log`, cut off after the second step).

- **Before the title takes the GPU, its surface writes don't stick.** Both times `D1GRPH_PRIMARY_SURFACE_ADDRESS` read
  0 straight after the write and 0x60EC. After taking the GPU, earlier runs showed the address sticking.
- **Nothing showed on screen** at any step, opaque white from either buffer included.

So the display is held on the kernel's placeholder while XAM has the GPU, and shows nothing the title gives it after.

## What XAM writes to show the Guide (2026-10-10)

`tests/probe_guide.c` watched wider register blocks while the Guide opened and closed, then snapshotted them around
`VdInitializeEngines` and `VdShutdownEngines` (log: `results/2026-10-10-probe-guide-2/xsdk.log`).

- **XAM also writes its surface to 0x2840 and 0x2844,** alongside D1GRPH: 0x2840 took 0x01C10000, the same address as
  0x6110, and 0x2844 took 864, the same as the pitch. libxenon writes its framebuffer address and width there too
  (libxenon:drivers/xenos/xenos.c:384-386). The earlier probes never wrote them.
- **XAM loads the LUT as the Guide appears.** `DC_LUT_RW_INDEX` (0x6488) counted up over about 200 ms while 0x648C,
  0x6490 and 0x6494 took a rising ramp (0x6494 went 0x00100401, 0x00401004, 0x00601806, ...), so the Guide fades in
  through the LUT. At start `DC_LUT_WRITE_EN_MASK` read 7 and `DC_LUT_AUTOFILL` 2.
- **0x28BC, 0x28C0 and 0x28C4** went from 0x00010000, 0x01B8D000 and 0x01000000 to 0, 0x1000 and 0x10000 while the Guide
  showed, and back after.
- **`VdInitializeEngines` changed none of these registers.** `VdShutdownEngines` handed the display back to XAM, which
  set a 1280x720 surface at 0x1FC59000 in both 0x2840 and 0x6110, with 1280 in 0x2844, 0x6120 and 0x6134 and 720 in
  0x6138.

So a surface shows when 0x2840 and 0x2844 match D1GRPH, and the LUT may be holding the screen black in between.

## A title's buffer on screen (2026-10-10)

`tests/probe_display.c` took the GPU, then set the surface address and pitch in both 0x2840/0x2844 and D1GRPH and set
0x60EC, keeping the kernel's 640x480 mode, and showed a buffer the CPU filled. The user reported what the screen showed.

- **White didn't show with the surface set alone.**
- **White showed once the LUT was filled** with libxenon's autofill: `DC_LUTA_CONTROL` 0, `DC_LUT_RW_INDEX` 0,
  `DC_LUT_RW_MODE` 0, `DC_LUT_WRITE_EN_MASK` 7, `DC_LUT_AUTOFILL` 1, then waiting for its bit 1
  (libxenon:drivers/xenos/xenos.c:419-424). So the LUT holds the screen black while a title runs, and every earlier
  test was black because of it.
- **Pixels are 0xAARRGGBB stored little-endian.** The words 0x0000FFFF, 0x00FF00FF and 0xFF0000FF, written by the
  CPU, showed red, green and blue.
- **The display reads libxenon's 32x32 tile layout** (libxenon:drivers/console/console.c:60-61): red and blue stripes
  drawn with it showed as stripes, with the white diagonal in place.

Whether 0x2840 and 0x2844 are needed as well as D1GRPH isn't settled, since the surface was set in both throughout.

## A first resolve runs but copies only parts (2026-10-10)

`tests/probe_resolve.c` showed a CPU-filled magenta 640x480 front buffer, then resolved four times in libxenon's way,
each clearing eDRAM to the next colour (white, red, green, black) while copying the previous one into the front buffer
(log: `results/2026-10-10-probe-resolve/xsdk.log`).

- **Every stage finished:** state and the inline vertex shader, the rectangle draw in copy mode, `EVENT_WRITE`
  `CACHE_FLUSH` and the coherency wait all let their fences land. So with render-target state set, the cache flush
  doesn't wedge the GPU.
- **The copies landed only in parts.** Pixels 0 and 100000 stayed magenta through the first two resolves and read 0
  after the third and fourth. The user saw no white, red or blue, and at one point magenta and green in stripes.
- **The console froze when Aurora came back.** The probe left `RB_MODECONTROL` at 6 (copy) and had loaded its shader at
  instruction address 0, either of which could have upset XAM's device.

## The resolve's tile map (2026-10-10)

`tests/probe_resolve.c` repeated the four resolves and mapped the front buffer, one character per 4 KB tile, after
each one and half a second later (log: `results/2026-10-10-probe-resolve-2/xsdk.log`, xbWatson's copy).

- **The first two resolves wrote nothing:** every tile stayed magenta, though all their stages' fences landed.
- **The third wrote zero over the whole frame,** not the white or red the earlier clears should have left in eDRAM, so
  when a copy does land its rectangle covers the screen. After the fourth every tile still read zero.
- **Nothing changed in the half second after each map,** so the writes don't arrive late in time.

## Only the third resolve writes (2026-10-10)

`tests/probe_resolve.c` refilled the front buffer with magenta before each of four resolves, cleared white, white, green
and green, and mapped the buffer after each resolve and again after a second cache flush and coherency wait (log:
`results/2026-10-10-probe-resolve-3/xsdk.log`, xbWatson's copy).

- **The first, second and fourth resolves wrote nothing:** every tile stayed magenta, though all their fences landed.
- **The third changed every tile, but not to one value:** each tile held more than one value, and none started with a
  value outside the probe's colours and zero. The map can't tell part-white, part-magenta tiles from others.
- **The second flush changed nothing,** so the missing writes aren't held in a cache.
- **The third resolve was also the only one to write in the previous run,** whose holds and stages differed, so the
  pattern follows the resolve's position in the sequence, not the time or the ring position.

## Every third resolve writes (2026-10-10)

`tests/probe_resolve.c` flushed the fetch constants after setting them, as libxenon does
(libxenon:drivers/xenos/xe.c:1146), then ran eight resolves, clearing white, white, green, green, blue, blue, white and
white, with the front buffer refilled with magenta before each (log: `results/2026-10-10-probe-resolve-4/xsdk.log`).

- **Only the third and sixth resolves wrote.** The third wrote 0 to all 307200 pixels, the sixth 0x00FF00FF (green) to
  all of them. Every other resolve left every pixel magenta, though all fences landed.
- **A skipped resolve skips its clear too.** The sixth copied the green the third cleared to, not the blue the fifth
  should have left, so the skipped resolves did nothing in eDRAM.
- **The green clear value 0xFF00FF00 landed as the word 0x00FF00FF,** the green the display shows.
- **The fetch constant flush didn't change the pattern.**
- **The console still froze on the way back to Aurora,** though the probe set `RB_MODECONTROL` back to 4 first.

## libxenon's set-up makes the resolves work (2026-10-10)

`tests/probe_resolve.c` took the GPU, sent libxenon's set-up (`tests/libxenon_gpu.h`, from
libxenon:drivers/xenos/xe.c:398-568) in fenced stages, resolved eight times as libxenon does, and wrote back the
registers it had changed before giving the GPU up (log: `results/2026-10-10-probe-resolve-5/xsdk.log`, cut off during
the second resolve). The user reported that the run went perfectly and the console returned to Aurora.

- **Every set-up stage's fence landed,** `ME_INIT` included, with the console's own microcode.
- **The first two resolves wrote,** where before only every third did: the first copied zero over all 307200 pixels,
  and the second the white the first cleared to.
- **The kernel leaves the shader unit set up differently from libxenon.** Read after taking the GPU, then after the
  set-up: `SQ_GPR_MANAGEMENT` 0x00040400, then libxenon's 0x00070100; `SQ_INST_STORE_MANAGMENT` 0x00000800, then
  0x00010800; `PA_CL_ENHANCE` 7, then 3. `SQ_FLOW_CONTROL` read 0x04007000 both times, though the set-up wrote
  0x04000000. `UNKNOWN_0A02` to `0A05`, `UNKNOWN_0E42` and `RB_BC_CONTROL` already held libxenon's values.
- **Drawing registers don't read back what a title writes.** From 0x2000 up, each read the same before and after the
  set-up, even where it wrote another value: the window scissor read 16x16 after the set-up set 1024x720. The values
  look like XAM's last resolve: a 1280-pixel surface and a 1280x720 copy to 0x1C0F2000.
- **The console didn't freeze on the way back to Aurora.** The probe wrote back what it read, which for drawing
  registers is that XAM-like state.

Which part of the set-up makes every resolve write, and which write-back stops the freeze, isn't isolated.

## The SDK resolves every frame; the display's size is the viewport (2026-10-10)

The test title ran `tests/test_present.c` after `tests/test_gpu.c` had opened and closed the GPU: it opened the GPU
again through the SDK, which now sends libxenon's set-up, and resolved 240 frames into two front buffers in turn,
fading the clear colour (log: `results/2026-10-10-milestone-5/xsdk.log`).

- **Every resolve landed:** all 240 fences, and every frame after the first held the colour the resolve before it
  cleared to, at its first, middle and last pixel. So the set-up works from `xsdk_gpu_open`, also on a second open.
- **The display check failed.** `xsdk_display_open` took the size from `D1GRPH_PITCH` and `D1GRPH_Y_END`, which XAM
  sets to 1280x720 when `VdShutdownEngines` hands the GPU back ("What XAM writes to show the Guide"). Through that
  hand-back `AVIVO_D1MODE_VIEWPORT_SIZE` (0x6584) stayed 0x028001E0, 640 wide in its high half and 480 high
  (`results/2026-10-10-probe-guide-2/xsdk.log`), so the display still scaled up a 640x480 surface. With the size
  wrong, the test showed nothing.
- **The other 39 checks passed.**

## A cleared screen, milestone 5 (2026-10-10)

The test title passed all 41 checks on the console and exited with status 0; the log is in
`results/2026-10-10-milestone-5-2/xsdk.log`.

- **`xsdk_gpu_open` sets the GPU up as libxenon does,** after taking it, on the title's second open as well as its
  first, and `xsdk_gpu_close` gives it back each time.
- **`xsdk_display_open` reports the viewport's 640x480,** though the earlier close had left XAM's surface registers
  at 1280x720, and fills the LUT.
- **Every frame clears, resolves and presents.** Over 240 frames the clear colour faded between magenta and green;
  each frame resolved into one of two front buffers in turn, held the colour the resolve before it cleared to at its
  first, middle and last pixel, and was shown with `xsdk_display_show`, whose 0x60EC write the display took every
  time.

The user didn't report what the screen showed in this run; earlier runs showed CPU-filled buffers through the same
registers ("A title's buffer on screen").

## A resolve keeps colours; the clear colour is packed red-low (2026-10-10)

`tests/probe_resolve.c` cleared eDRAM through the SDK with `RB_COLOR_CLEAR` 0xFFFF0000, resolved it into a magenta
front buffer and showed it (log: `results/2026-10-10-probe-resolve-6/xsdk.log`).

- **Every pixel held 0xFF0000FF, and the user saw blue,** as the CPU's 0xFF0000FF shows ("A title's buffer on screen").
- **Xenia's model gives the same:** a `k_8_8_8_8` target packs red into the low byte (xenia:
  src/xenia/gpu/shaders/pixel_formats.xesli:113-116, 289-292), the clear writes `RB_COLOR_CLEAR` as that packed value,
  and the copy's swap bit, which libxenon sets, swaps red and blue on the way out (shaders/resolve.xesli:632-635). By
  that model 0xFFFF0000 is a blue clear, and the green run's 0x00FF00FF comes out too.

So the copy and the display carry a colour through unchanged, and only the clear value needs red and blue swapped
from 0xAARRGGBB. What a shader draws isn't covered yet.

## The packed clear shows red (2026-10-10)

With `xsdk_gpu_resolve` swapping red and blue in the clear colour, the same probe's red clear stored 0x0000FFFF in all
307200 pixels, the word the display shows as red (log: `results/2026-10-10-probe-resolve-7/xsdk.log`). The user didn't
report the screen in this run; the log names the colour the stored word shows.

## Milestone 5 with a red and blue fade (2026-10-10)

The test title, with the present test fading between red and blue, passed all 41 checks, exited with status 0 and
returned to Aurora (log: `results/2026-10-10-milestone-5-3/xsdk.log`). Every frame held the colour cleared before it,
red and blue in the right places, so `xsdk_gpu_resolve`'s packing of the clear colour holds across 240 frames.

## The first triangle draws nothing (2026-10-10)

The test title ran `tests/test_triangle.c`: shaders from `tools/gen_triangle_shaders.py` loaded with `IM_LOAD`, its
size given in dwords, the vertex fetches patched, and 120 frames each drawing a triangle with `DRAW_INDX_2` and
resolving it into a front buffer (log: `results/2026-10-10-milestone-6/xsdk.log`, xbWatson's copy).

- **Every frame's fence landed, but nothing was drawn:** the pixels inside the three corners held the clear colour in
  every frame checked, and the last frame had no pixel other than the clear colour. The same commands draw the triangle
  in Xenia, all 86160 pixels of it.
- **The log ended after the triangle test's last check,** before the GPU was closed and before the test count and exit
  lines, so the title seems to have stopped there.
- **The vertex shader matches xost's passthrough,** which xost has drawn on hardware through libxenon: the same control
  flow, `SQ_PROGRAM_CNTL` and fetch encoding, apart from its registers and the fields the patch rewrites. libxenon
  gives `IM_LOAD` the shader's size in bytes (libxenon:drivers/xenos/xe.c:1210-1215, 1383), where the SDK gave dwords.

## The triangle doesn't draw however its shaders load (2026-10-10)

`tests/probe_triangle.c` drew the triangle four ways into the same front buffer, each after a resolve that cleared eDRAM
to grey, and counted the pixels other than grey (log: `results/2026-10-10-probe-triangle/xsdk.log`).

- **With a fence before the resolve, every pixel came out grey,** whether `IM_LOAD` was given the size in bytes or the
  shaders were loaded inline with `IM_LOAD_IMMEDIATE`, as the resolve loads its own. So the draw writes nothing, and
  how the shaders are loaded isn't why.
- **Without that fence, every pixel held zero,** with `IM_LOAD`'s size in dwords and in bytes. The buffer was never
  filled by the CPU, so the resolve after the draw most likely wrote nothing.
- **The patched vertex fetches read back as written.**
- **The console froze after returning to Aurora,** as it did before the probe gave back the registers it changed.

## No single draw setting makes the triangle draw (2026-10-10)

`tests/probe_triangle.c` drew the triangle six ways, each from an idle GPU with eDRAM cleared to grey, its shaders
loaded inline, a fence after the draw, and a magenta front buffer (log: `results/2026-10-10-probe-triangle-2/xsdk.log`).

- **None drew a pixel:** not as the SDK draws, nor with z 0.5, clipping off, `DRAW_INDX` with a viz query token,
  vertex fetch constant 0, or screen-space corners with no viewport transform or clipping, as the resolve draws.
- **After three of them the resolve wrote nothing,** leaving every pixel magenta, though a fence came between the draw
  and the resolve: z 0.5, `DRAW_INDX` and fetch constant 0. After the other three it wrote grey.

## libxenon's frame draws the triangle (2026-10-10)

`tests/probe_triangle.c` drew the triangle with the frame that xost's libxenon triangle app sends, as libxenon's `xe.c`
builds it on the host, then with one of its differences from the SDK's draw undone at a time, then as the SDK draws
(log: `results/2026-10-10-probe-triangle-3/xsdk.log`).

- **libxenon's frame drew the triangle:** 86160 pixels, as in Xenia, red at the top. So the kernel's command processor
  microcode, the SDK's memory and the generated shaders can draw it.
- **It still drew with each difference undone:** `IM_LOAD`'s size in dwords, no depth test, `DRAW_INDX_2`, and the ALU
  or the bool and loop constants left alone.
- **The SDK's draw, run last, drew nothing, and the resolve after it wrote nothing,** though every fence landed. The
  earlier draws had zeroed the constants and pointed vertex fetch constant 95 at the same vertices, so what was left to
  differ was those five settings together and how the SDK sets the fetch constant: its two dwords alone from `0x48BE`,
  its size in dwords, 0 in `SHADER_CONSTANT_FLUSH_FETCH_1` where libxenon writes 0x00025000, its own vertex cache
  invalidate, and the order of its packets.
- **The console froze after returning to Aurora.**

## Fetch constants go in whole groups (2026-10-10)

`tests/probe_triangle.c` drew the triangle the SDK's way with one of libxenon's ways of setting the vertex fetch
constant at a time, each followed by libxenon's whole frame as a check (log:
`results/2026-10-10-probe-triangle-4/xsdk.log`).

- **The SDK's draw drew the triangle, all 86160 pixels, when fetch constant 95 went with the two before it,** the six
  dwords from `0x48BA`, or with every fetch constant. libxenon writes fetch constants in such groups of three
  (libxenon:drivers/xenos/xe.c:1334-1355), and the resolve writes fetch constant 0 with its group.
- **Written alone, its two dwords from `0x48BE` drew nothing:** with its data on 16 bytes in the ring, with 0x00025000
  in `SHADER_CONSTANT_FLUSH_FETCH_1`, with its size in bytes, without the vertex cache invalidate, with the target set
  after it, and as the SDK drew.
- **Lone writes broke drawing in threes.** After the first, no resolve wrote anything and libxenon's frame drew nothing,
  until the third, whose own triangle didn't draw but whose resolve wrote; libxenon's frame drew again after it. It
  went the same way twice, and the whole-group writes in between, libxenon's frame with the SDK's five settings among
  them, didn't change it. So the GPU seems to take fetch constants six dwords at a time, whatever their address.
- **That fits the earlier runs:** the previous one's single lone write, last, left the resolve writing nothing and
  Aurora frozen, and in the one before (`results/2026-10-10-probe-triangle-2/xsdk.log`) it fits five of the six
  resolves; the first wrote though it came after a lone write.

## The triangle draws (2026-10-10)

The test title ran `tests/test_triangle.c` with `xsdk_gpu_set_vertices` writing the fetch constant's whole group (log:
`results/2026-10-10-milestone-6-2/xsdk.log`, xbWatson's copy).

- **Every triangle check passed:** the last frame drew 86160 pixels, as in Xenia, every frame had red, green and blue
  corners on the clear colour, and the GPU closed.
- **The present test failed, though it passed in the first triangle run and nothing it runs has changed since.** In the
  four frames logged, the first pixel held the colour the resolve before had cleared to, so the middle or the last pixel
  didn't; the test logged only the first.

## Resolves without draws miss the last pixel (2026-10-10)

The test title ran again, the present test now logging the three pixels it checks (log:
`results/2026-10-10-milestone-6-3/xsdk.log`, xbWatson's copy).

- **The triangle drew and passed again;** the user saw it, and the console went back to Aurora without freezing.
- **Every present frame missed its last pixel,** (639, 479), which still held the zero the buffer was allocated with,
  while the first and middle pixels held the colour cleared before. The present test passed in the runs before the
  last two.
- **The triangle's resolves in the same run wrote that pixel:** unwritten, it would have counted as drawn, and the last
  frame drew 86160 pixels, as in Xenia. The triangle sets its draw state before resolving, where the present test only
  resolves.
- **A missing last row and column is what the other pixel centre would give.** The resolve's corners sit half a pixel
  up and left (libxenon:drivers/xenos/xe.c:1074-1081); Xenia adds that half pixel back for Direct3D centres, which the
  set-up selects, and without it coverage stops one pixel short of the right and bottom
  (xenia:src/xenia/gpu/draw_util.cc:837-867).

## A resolve needs multisampling off (2026-10-10)

`tests/probe_resolve.c` read the drawing registers XAM left, resolved as the present test does, then after setting
`PA_SC_AA_CONFIG` to 0, then after the rest of the triangle's target state (log:
`results/2026-10-10-probe-resolve-8/xsdk.log`).

- **XAM had left multisampling set up:** `PA_SC_AA_CONFIG` 0x0000C003, next to libxenon's 4x value 0x0000C004
  (libxenon:drivers/xenos/xe.c:1738-1740), and `RB_SURFACE_INFO` with 4x samples, which the resolve overwrites.
- **With it, each resolve wrote half the pixels:** 153600 were left unwritten, the first at (8, 0), with 320 of the last
  row's 640 and all of the last column, as if every other 8-pixel column were skipped. The present test's first and
  middle pixels, both at x = 0, fell in columns that were written.
- **With `PA_SC_AA_CONFIG` 0, every resolve wrote every pixel,** and after the triangle's target state too, which sets
  it to 0 with the rest.

The resolve now sets `PA_SC_AA_CONFIG` to 0, and the SDK gives XAM's value back when it closes the GPU.

## A triangle, milestone 6 (2026-10-10)

The test title passed all 51 checks on the console, exited with status 0 and went back to Aurora without freezing; the
log is in `results/2026-10-10-milestone-6-4/xsdk.log`.

- **xost-encoded shaders, loaded with `IM_LOAD`, drew a coloured triangle from a vertex buffer.**
  `xsdk_shader_load` took the containers from `tools/gen_triangle_shaders.py`, `xsdk_shader_set_vertex_layout`
  patched the vertex fetches for a float4 position and colour, and `xsdk_gpu_set_vertices` wrote vertex fetch constant
  95 with its group. Each of 120 frames had red, green and blue corners on the clear colour, and the last drew 86160
  pixels, as in Xenia. The user saw the triangle in the run before.
- **The present test passed again** with the resolve setting `PA_SC_AA_CONFIG` to 0: every frame held the colour
  cleared before it.

## A textured quad, milestone 7 (2026-10-10)

The test title passed all 66 checks on the console and exited with status 0; the log is in
`results/2026-10-10-milestone-7/xsdk.log`, and `capture.png` beside it is the user's capture of one frame.

- **A texture the SDK uploaded sampled correctly, linear and then tiled.** A 160x96 ARGB texture whose texels all
  differ, drawn one texel to a pixel at (64, 48), held every texel in place in the last of 60 frames each way, with the
  pixels around it the clear colour, and five texels checked in every frame. So the fetch constant, the ARGB swizzle,
  the 256-byte linear pitch, `xsdk_tiled_2d` and the float2 texture coordinates all hold on the console.
- **The capture shows the texture as written:** navy at the top left, red rising to the right and green downwards, and
  blue wrapping from 255 to 128 along x + y = 128, from texel 128 on the top edge to about texel 33 on the bottom.

## Linear and tiled textures side by side (2026-10-10)

The texture test now draws both textures in every frame, the tiled one with red and green swapped and its blue bands
slanted the other way, so a capture shows both and a quad sampling the other texture fails. The test title passed all
64 checks and exited with status 0 (log: `results/2026-10-10-milestone-7-2/xsdk.log`, the user's capture beside it).

- **Each quad held its own texture, every texel in place:** no wrong pixel in either quad or its border in the last of
  120 frames, with the texture rebound to slot 0 between the two draws of each frame.
- **The capture shows both as written:** on the left the linear texture as before; on the right green rising to the
  right and red downwards, a blue seam from the top left corner to texel (95, 95) on the bottom edge, and a teal corner
  cut off from texel 128 on the top edge to row 31 on the right.

## Render to texture, milestone 8 (2026-10-10)

The test title passed all 77 checks on the console and exited with status 0; the log is in
`results/2026-10-10-milestone-8/xsdk.log`, with the user's captures of each half beside it.

- **A scene resolved into a texture that a second pass sampled.** Each of 120 frames drew the source texture mirrored
  over a 160x96 render target, resolved it into that tiled texture with `xsdk_gpu_resolve_texture`, then drew the
  source and the render target side by side on screen. The rendered quad held the mirrored source in every texel in
  the last frame of each half, and its sampled texels in every frame, through the switch from mirroring left to right
  to top to bottom at frame 60, so no frame sampled a stale texture. The render target started magenta.
- **The resolve's 8-in-32 byte order leaves ARGB words,** as for libxenon's ARGB surfaces
  (libxenon:drivers/xenos/xe.c:1104-1108): sampled with the same fetch constant as an uploaded texture, every colour
  came out right.
- **Both resolve sizes in each frame came out right** with a rectangle kept for each; this run didn't time whether the
  CPU still waited.
- **The captures show both halves as written:** first the right quad mirrored left to right, magenta at its top left
  and its seam from about texel 32 at the top to 127 at the bottom; then mirrored top to bottom, navy at its bottom
  left and its seam from texel 33 at the top to 128 at the bottom.

## The vblank callback, flips and giving the callback back (2026-10-10)

`tests/probe_vblank.c` took the GPU, set its own graphics interrupt callback, recorded each call for about a second
with the time base and the display registers that change each frame, flipped between two greys ten times, then
cleared the callback, gave the GPU back and watched XAM's write pointer (log:
`results/2026-10-10-probe-vblank/xsdk.log`).

- **The callback comes twice a frame, 930 us apart, always with source 0.** The first of each pair read 0x6530 452,
  0x6534 0 and 0x6544 0x00110000; the second read 0x6530 480, 0x6534 0x1001 and 0x6544 0x11. Each pair started 16624
  us after the last. Exactly half the calls, 67 of 134, had 0x6534's bit 12 set, which libxenon reads as vblank
  (libxenon:drivers/xenos/xe.c:2141-2145). 0x6530 looks like the current line: 480 is the mode's height, where vblank
  starts, so the first call comes 28 lines before vblank and the second as it starts.
- **Nothing shows a flip waiting for vblank.** After each address write, 0x60EC read 0 again at once, at any point in
  the frame, and `D1GRPH_UPDATE` read 0, so nothing held the flip for vblank. The user saw the greys alternate. So a
  flip that mustn't tear has to be written during vblank.
- **Giving the GPU back with the title's callback cleared lets XAM run again.** After `VdSetGraphicsInterruptCallback(0,
  0)` and `xsdk_gpu_close`, XAM's write pointer moved 15 times in 250 ms, as after a run that never set a callback
  ("Giving the GPU back with VdShutdownEngines").

## The pacing modes pace, but small vertex invalidates don't take (2026-10-10)

The test title ran `tests/test_pacing.c`: each of four present modes for 120 frames, a moving bar its own colour per
mode, the GPU open throughout and the bar's vertex buffer allocated anew for each mode (logs:
`results/2026-10-10-milestone-9/xsdk.log`, then `results/2026-10-10-milestone-9-2/xsdk.log` with the misses logged).

- **Every mode paced exactly.** Three and two buffers flipped 120 frames over 120 vblanks, and at a swap interval of 2
  over 240, with no vblank missed and no flip late; the longest wait for a buffer was 17949 us at interval 1 and 33909
  us at 2. Every earlier test passed too, now that the SDK's waits return an error when they time out.
- **After the first mode, the bar came out in the previous mode's colour.** The yellow mode's second frame held white
  (0xFFFFFFFF), the cyan mode's yellow and the magenta mode's cyan, and the first frame of each a blend between the two;
  40 of each mode's 118 checked frames were wrong. The new vertex buffer most likely sat in the memory the last mode's
  had used, and the vertex cache still held it, though `xsdk_gpu_set_vertices` invalidated the vertex cache over the 192
  bytes each draw read. The first mode followed `xsdk_gpu_open`'s invalidate of all memory and came out right.
- **libxenon never invalidates less than a page:** it rounds a lock up to 4096 bytes and adds a page to its vertex
  pool's range (libxenon:drivers/xenos/xe.c:766, 1871). So the SDK now rounds every invalidate out to whole 4 KB pages.

## Whole-page invalidates keep vertices fresh (2026-10-10)

With `xsdk_gpu_put_coherency` rounding every range out to whole 4 KB pages, the test title passed all 103 checks and
exited with status 0 (log: `results/2026-10-10-milestone-9-3/xsdk.log`).

- **Every pacing mode drew every frame's own bar:** no reused buffer held the wrong frame in any of the four modes,
  where the yellow, cyan and magenta modes had 40 each before.
- **The pacing held as before:** 120 flips over 120 vblanks at interval 1 and over 240 at interval 2, none missed or
  late, in all four modes.

## Ten minutes of paced frames, milestone 9 (2026-10-10)

`tests/pacing.c` presented the moving bar for 10 minutes with three front buffers and a flip every vblank, checking
each frame's bar as its buffer came back (log: `results/2026-10-10-milestone-9-4/xsdk.log`).

- **Every frame flipped on its own vblank for the whole run:** 36093 frames, 36093 flips over 36094 vblanks, no vblank
  missed and no flip late, the minute-by-minute counts rising by 3609 each minute. The longest wait for a buffer was
  17950 us, one vblank, as the pacing throttles the title.
- **Every frame after the first held its bar.** Frame 0 didn't, and the title didn't log its pixels. Its background is
  whatever eDRAM held before the first resolve cleared it, which is why the other tests skip frame 0; the pacing checks
  now skip it too and log the pixels of any miss.

## Frame pacing, milestone 9 (2026-10-10)

`tests/pacing.c` ran its 10 minutes again with frame 0 left out of the bar check, and passed all 8 checks with status 0
(log: `results/2026-10-10-milestone-9-5/xsdk.log`).

- **Frames were paced to vsync through the interrupt callback and fences for 10 minutes:** 36094 frames, 36094 flips
  over 36095 vblanks, a flip every vblank, with three front buffers, so two frames in flight behind the one shown.
- **No stalls and no tearing, by the run's own counts:** no vblank missed, so no frame shown twice, and no flip still
  being written once the scan had left vblank.
- **Every frame from frame 1 on held its own bar** when its buffer came back, 3609 more frames each minute, and the
  longest wait for a buffer stayed at 17950 us, one vblank.

The console went back to Aurora normally afterwards, the user reported.

## The console outputs 1920 pixels wide; a 1280x720 surface shows 1:1 (2026-10-10)

`tests/probe_720p.c` showed a 1280x720 image in the kernel's 640x480 viewport, then with libxenon's scaler settings for
a mode shown 1:1 (libxenon:drivers/xenos/xenos.c:381-417), logging the display's registers, and put them back (log:
`results/2026-10-10-probe-720p/xsdk.log`, with the user's captures of both phases).

- **The output is 1920 pixels wide, not 1280.** `D1CRTC_H_TOTAL` read 0x897 and `D1CRTC_H_BLANK_START_END` 0x00BF083F,
  which libxenon writes as the total less one and as the sync offset over the active width plus that offset
  (xenos.c:344-346): 2200 pixels a line, 1920 of them shown. The kernel's scale factors fit 1920 by 1080: 0x00555560
  and 0x0071C720 in 0x65B4 and 0x65C4, a third and four ninths if 0x01000000 is 1.0, as libxenon writes it for 1:1,
  so 640 to 1920 and 480 to 1080. An earlier run's `VdQueryVideoMode` reported 1280x720 ("VdSwap writes only a
  fence"), so the console's output setting has changed since or differs with the user's capture card.
- **libxenon's settings show the surface 1:1.** With the scaler off, the viewport at 1280x720, 0x65E8 at 319 and the
  scale registers at 1.0, the whole image showed unscaled, its one-pixel border and all four corner squares in view,
  in the top left two thirds of the screen with black around it, as 1280x720 sits in 1920x1080.
- **The kernel's 640x480 scaling showed the top left 640x480 of the image,** twice as wide and half again as tall.
- **0x6024, libxenon's `AVIVO_D1CRTC_V_BLANK_START_END`, changed between reads,** and `D1CRTC_V_TOTAL` read 0, which
  libxenon also writes (xenos.c:340), so neither gives the output's height.
- **Putting the registers back returned the display to the kernel's mode,** and the console went back to Aurora.

## Scaling 1280x720 by hand scrambles it (2026-10-10)

`tests/probe_720p.c` showed the 1280x720 test image scaled to the console's output with the scaler on, the kernel's
tap control 0x505, scale factors of source over output in 8.24, and start phases from a formula that fits the
kernel's and libxenon's values, then with both phases 0 (log: `results/2026-10-10-probe-720p-2/xsdk.log`, with one
capture whose phase the user didn't say).

- **The output is 1920x1080 progressive by the kernel's own account:** `VdQueryVideoMode` gave 1920x1080, not
  interlaced, widescreen and hi-def, and `VdGetCurrentDisplayInformation` a 640x480 front buffer on a 1920x1080
  display, its scaler parameters a 640x480 source rectangle, a 1920x1080 output and filter type 1 both ways.
- **The scaler registers drop low bits.** The scale written as 0x00AAAAAA read back 0x00AAAAA0, and the phase written
  as 0x3D555 read 0x3D400, which fits the kernel's own values ending in zeros.
- **The image came out scrambled.** The capture shows a narrow copy of the image's frame, its border and all four
  corner squares striped every other line, across the left sixth of the screen, and the middle blocks larger in the
  middle. The same surface showed whole at 1:1 the run before, so the scaler, not the surface, is set wrong; which of
  its registers isn't known. The kernel builds the scaler's packets itself in `VdInitializeScalerCommandBuffer`
  (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:270-293), which the next probe asks it for.
- **Putting the registers back returned the kernel's scaling,** and the console went back to Aurora.

## The kernel's scaler packets name the registers but carry zeros (2026-10-10)

`tests/probe_720p.c` asked `VdInitializeScalerCommandBuffer` for 1280x720 to the reported 1920x1080 with the
kernel's own filter settings, logged the packets, applied their register writes through MMIO, showed the test image
for 12 seconds and wrote back what it had read from each register (log: `results/2026-10-10-probe-720p-3/xsdk.log`).

- **The call returned 43 dwords: type-0 headers over the registers libxenon sets, plus more, every value 0.** In
  order: 0x6590-0x6594 (scaler enable and taps), 0x6528, 0x6580-0x6584 (viewport start and size), 0x6120-0x6138
  (`D1GRPH` pitch to Y end), 0x6044-0x6050, 0x65E8, 0x65A0, 0x65B0-0x65C8 (taking in 0x65BC, which nothing here had
  written), 0x65DC and 0x6578. Six dwords followed that don't parse as packets: 0xFFFF995F, 0x195E, 0x00020000,
  0xFFFF995F, 0x1964, 0. Whether the values come later from elsewhere, or the arguments as Xenia guesses them are
  wrong, isn't known.
- **Applying the zeros blanked the screen,** viewport and surface sizes included, and it stayed black from launch to
  exit. That was the probe's fault: it applied the writes without checking them.
- **The console didn't come back to Aurora.** Besides the scaler, the probe had written back values it read from
  0x6044-0x6050 and 0x6578, which libxenon writes as 0 and may not read back what was written; that, or the zeros
  themselves, most likely left the display unable to recover.

So the scaler set the kernel builds includes 0x65BC and 0x6578, which the hand-set scaling left alone.

## 0x65BC isn't it, and calling the kernel's scaler builder breaks the way back (2026-10-10)

`tests/probe_720p.c` logged the kernel's starting values for its scaler set, asked `VdInitializeScalerCommandBuffer`
for its packets only to log them, then scaled the 1280x720 image as before and again with 0x65BC set, writing only
scaler registers (log: `results/2026-10-10-probe-720p-4/xsdk.log`, with one capture whose phase the user didn't say).

- **The kernel's own 640x480-to-1920x1080 setup:** 0x6590 1, 0x6594 0x505, 0x6528 0, viewport start 0 and size
  640x480, `D1GRPH` pitch and X end 640 and Y end 480, 0x65E8 0x9F, 0x65A0, 0x65B0, 0x65BC and 0x65DC 0, 0x65B4
  0x00555560, 0x65B8 0x3AA00, 0x65C0 0x01000000, 0x65C4 0x0071C720 and 0x65C8 0x3B800.
- **The packets carried zeros again,** the same 43 dwords, and the buffer passed as the tenth argument stayed zero.
- **0x65BC started at 0,** matching none of the scale or phase values, so the probe gave it the horizontal scale as a
  guess. The capture looks as scrambled as before; which phase it shows isn't known.
- **The console crashed after the title exited,** the screen black, and xbWatson reported it restarting. The probe
  wrote only registers the hand-set scaling had written, which went back to Aurora, plus 0x65BC, and put them back.
  The two runs that called `VdInitializeScalerCommandBuffer` both failed to return to Aurora, and the one that didn't
  call it returned, so the call itself may change the kernel's display state, perhaps from arguments it reads
  differently from Xenia's guess; that isn't shown. The SDK won't call it.
