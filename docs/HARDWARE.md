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
- **Time.** A 250 ms `sleep_for` measured 249 ms and 250 ms on `steady_clock` in two runs. That was too coarse to show
  that `KeQueryPerformanceFrequency` overstates the time base's rate by 0.25%, nor that the sleep can end early (see
  "The time base runs at 49.875 MHz (2026-10-11)"). `system_clock` reads the right date from `KeQuerySystemTime`.
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

## What AMD's code says about the scaler (2026-10-10)

Not a hardware run: what public AMD sources say about this display block, to read the probes against. AMD's register
guides for the M56 and RV630 (x.org/docs/AMD/old) give the CRTC, graphics and LUT registers at the same offsets as
here (`D1CRTC_H_TOTAL` 0x6000, `D1GRPH_*` 0x6100-0x614C, `D1MODE_MASTER_UPDATE_LOCK` 0x60E0) but leave out
0x6500-0x65FF, the viewport and the scaler.

- **radeonhd scales on R5xx without scale factors or phases** (xf86-video-radeonhd:src/rhd_crtc.c:767-786,
  `DxScaleSet`): 0x65A4 0x00010001, tap control 0x101, and 0x00030100 in both 0x65B0 and 0x65C0. Its names for those
  registers are marked as guesses (rhd_regs.h:205-213). Its scaler update lock is bit 16 of 0x65CC (rhd_regs.h:412-413).
- **AMD's later display code names the fields** (linux:drivers/gpu/drm/amd/include/asic_reg/dce/dce_8_0_sh_mask.h,
  `SCL_*`). Tap control holds the taps less one, vertical in bits 0-2 and horizontal in 8-11. A filter control's bit 8
  selects a built-in 2-tap filter, and bit 0 nearest. The automatic mode control's bits 0 and 16 make the hardware work
  out the scale factors. The coefficient select takes the tap pair in bits 0-3, the phase in 8-11 and the filter type
  in 16-18. The update lock is bit 16, and bit 0 is the pending flag, which libxenon and these probes write. Read that
  way, radeonhd asks for 2 taps each way with the built-in filter and automatic scale factors, and the kernel's 0x505
  is 6 taps each way.
- **The same code starts each filter at (scale + taps + 1) / 2** (linux:drivers/gpu/drm/amd/display/dc/dce/
  dce_transform.c, `calculate_inits`). That is the formula that fit the kernel's and libxenon's phases, with the taps
  one more than the field. It loads coefficients by filter type, phase and tap pair through `SCL_COEF_RAM_SELECT` and
  `SCL_COEF_RAM_TAP_DATA` (`program_multi_taps_filter`). It also refuses vertical taps that aren't fewer than the lines
  the line buffer holds, which is the buffer's size over the source width (`dce_transform_get_optimal_number_of_taps`).
  A 1280-wide source fits half as many lines as the kernel's 640-wide one.
- **The kernel's six trailing dwords read as coefficient loads.** xenon-emu names 0x6578 `D1SCL_COEF_RAM_SELECT` and
  0x657C `D1SCL_COEF_RAM_TAP_DATA` (XenosRegisters.h:919-920). The tail selects filter type 0, then type 2
  (0x00020000). After each selection comes a header for 0x657C with bit 15 set, which writes every dword to the one
  register (xenia:src/xenia/gpu/command_processor.cc:651-656), and with its count bits all set. xenon-emu names 0x65BC
  `D1SCL_HORZ_FILTER_INIT_CHROMA` (XenosRegisters.h:929), which fits it not mattering for RGB.
- **The scrambled capture fits source lines landing in the wrong place.** Read again (`results/2026-10-10-probe-720p-2/
  capture.png`), each output line in the corner squares' rows shows either the red square at the left edge, about the
  right size, or the green one about a sixth of the way across, never both. So the horizontal source position changes
  from line to line, which a filter or a scale factor alone wouldn't do. Too many vertical taps for the line buffer
  would, as lines overwrite each other.

## The built-in 2-tap filter scales 1280x720 to 1920x1080 whole (2026-10-10)

`tests/probe_720p.c` scaled the 1280x720 test image with the scaler on at 1:1, then to the output with fewer and fewer
vertical taps, with 2 horizontal taps, and with the built-in 2-tap filter each way, each variant on its own
background tint, writing only scaler registers (log: `results/2026-10-10-probe-720p-5/xsdk.log`, with the xbWatson
lines from the launch, a cropped capture of the purple variant and a full-frame screenshot of it at 1280x720).

- **Only the built-in 2-tap filter came out whole,** by the user's eye: tap control 0x101, bit 8 set in both filter
  controls (0x65B0 0x00000100, 0x65C0 0x01000100), scale 0x00AAAAA0 both ways, phases 0x1D400. In the full-frame
  screenshot the white border shows on all four edges, the corner squares sit in all four corners, and the middle
  blocks span x 448-832 and y 296-423, where the source has them.
- **Every variant on the kernel's coefficient table came out scrambled:** 6 taps each way at 1:1 and scaled, 4, 3 and 2
  vertical taps with 6 horizontal, and 2 horizontal with 6 vertical. So the line buffer limit on vertical taps
  (above) doesn't explain it alone: 2 vertical taps with 6 horizontal still scrambled. 2 taps each way on the table
  wasn't tried, so whether the table or the tap count is at fault isn't known.
- **The log can't tell scrambled from whole.** Every write read back as written, less the low bits the scaler drops,
  and the registers read after each variant didn't change: 0x6524 0, 0x65CC 0, 0x65D0 1, 0x65E0 0.
- **At start:** 0x6520 0, 0x6524 0, 0x6548 0, 0x6550 0xFF (libxenon writes 0xFF there, xenos.c:392), 0x65A0 and
  0x65A4 0, 0x65B0 0, 0x65BC 0, 0x65C0 0x01000000, 0x65DC to 0x65E4 0, 0x65E8 0x9F, 0x6CBC 3 and 0x7118 0x780, which is
  1920 and which xenon-emu names `DC_LB_MEM_SIZE` (XenosRegisters.h:952).
- **Putting the registers back returned the kernel's scaling,** and the console went back to Aurora.
- **Neither of the display's own checks can see the scaled image.** The M56 guide places `DCP_CRC_*` (0x6C80-0x6C94)
  on the data going into the line buffer, before the scaler, and calls `D1CRTC_PIXEL_DATA_READBACK` (0x6078) a debug
  register for one-shot clocking. A test of the scaled output needs a capture.

## 720p in the test title: one late flip at the first vblank (2026-10-10)

`build/xsdk_test.xex` ran with every GPU test at 1280x720 on the display's scaler, and the new scale test (log:
`results/2026-10-10-720p/xsdk.log`, with the xbWatson lines from the launch). 108 checks passed and 3 failed.

- **The 1280x720 front buffers work.** The present, triangle, texture and render tests passed every check, with 0
  wrong pixels in the texture and render tests and 258720 pixels drawn for a triangle of 259200.
- **The scaler took the SDK's settings and gave the kernel's back.** With the update lock at bit 16, the scale test read
  back tap control 0x101, the built-in filter bit in both filter controls, and the output's scale factors and phases
  100 ms after showing, then the kernel's values 100 ms after closing. 0x652C, which libxenon names
  `AVIVO_D1MODE_DESKTOP_HEIGHT`, read 1080.
- **The 3 failures were one late flip each, in the three pacing modes whose first flip came at vblank 1.** The mode
  whose first flip came at vblank 2 had none, and every mode missed no vblank and flipped every swap interval. At
  640x480 a first flip at vblank 1 wasn't late (`results/2026-10-10-milestone-9/xsdk.log`).
- **Why isn't shown.** The late check compares 0x6530, the line counter, with the front buffer's height. The counter
  runs in viewport lines: under the kernel's 480-line viewport its two calls a frame came 28 lines and 930 us apart,
  63 lines of the 1080p output ("The vblank callback, flips and giving the callback back"), so it stays below about
  500 there (1125 output lines times 480/1080). The SDK switches the scaler to 720 lines just before pacing starts. If
  the switch hadn't taken by the first vblank, the counter would still read on the old scale, under 720, and the flip
  would count as late whether or not it tore. So the first 1280x720 show now waits until the counter passes 720 and
  logs how long that took, and a late flip records its vblank and line.

## 720p at 1080p output passes clean, after waiting for the scaler (2026-10-10)

`build/xsdk_test.xex` ran again with the first 1280x720 show waiting until the line counter passed 720, and late flips
recording where they came (log: `results/2026-10-10-720p-2/xsdk.log`, with the user's screenshot of the scale test at
1280x720). All 111 checks passed, and the console went back to Aurora.

- **The counter took 18154 to 32559 us to reach 720 after each switch:** nine opens read 27794, 30122, 18154, 30123,
  22392, 32559, 30048, 29562 and 24900 us. That fits the new size taking at the next vblank, up to a frame (16.7 ms)
  away, and the counter then reaching 720 one active display later, 720 of 750 lines or about 16.0 ms: 16.0 to 32.7
  ms in all. So the first vblank after a switch still counts on the old scale, which is what the late flips needed.
  The next run's shortest wait, 16492 us, also falls in that range.
- **No flip was late.** Three of the four pacing modes made their first flip at vblank 1, where each had one late flip
  in the run before; all four had 0 missed and 0 late.
- **The scale test's screenshot shows the image whole and in place:** the blue-grey background, the white border on all
  four edges, the corner squares in all four corners, and the middle blocks at x 448-832 and y 296-423, as in the
  source.
- **Not covered by this run:** a title asking for `XSDK_DISPLAY_KERNEL`, which no test opened, and 1:1 at a 720p output,
  as the console outputs 1080p.

## The kernel's mode through the SDK, at 1080p output (2026-10-10)

`build/xsdk_test.xex` ran with the scale test also opening `XSDK_DISPLAY_KERNEL` (log:
`results/2026-10-10-720p-3/xsdk.log`). All 115 checks passed.

- **The kernel's mode opened at the kernel's 640x480 and left the scaler alone,** both 100 ms after showing a buffer
  and 100 ms after closing.
- **The 720p checks passed again,** with no late flip, and the counter reached 720 between 16492 and 32553 us after
  each switch.

## 720p at 720p output, and 720p done, item 10 (2026-10-10)

`build/xsdk_test.xex` ran with the console's video output set to 720p (log: `results/2026-10-10-720p-4/xsdk.log`,
with the user's screenshot of the scale test). All 115 checks passed, and the console went back to Aurora.

- **The kernel reported a 1280x720 output, and the SDK showed 1280x720 1:1 with the scaler off,** as libxenon does.
  The scale test read back the settings it expected for that, and `AVIVO_D1MODE_DESKTOP_HEIGHT` read 720.
- **The screenshot shows the whole 720p frame:** the blue-grey image, its border on all four edges, the corner squares
  and the middle blocks. The user's OBS canvas is 1920x1080, so a 720p signal sits 1:1 in its top-left, and the
  1280x720 screenshot shows the frame as its top-left 853x480.
- **Pacing at 720p output had no missed or late flip,** and the counter reached 720 19069 to 31152 us after each
  switch, inside the 16.0 to 32.7 ms worked out above.
- **The kernel's mode again opened at the kernel's size and left the scaler alone.**

With the 1080p runs above, item 10 is done. Titles get a 1280x720 front buffer by default: scaled to a 1080p output by
the display's built-in 2-tap filter, shown 1:1 on a 720p output, and the kernel's 640x480 when they ask for it. The
GPU tests pass at 1280x720, the captures show the image whole, and the console goes back to Aurora. Interlaced outputs
aren't tried and fall back to the kernel's mode.

## Ten minutes of paced frames at 720p (2026-10-10)

`build/pacing_test.xex` ran its 10-minute mode (3 buffers, a flip every vblank) with the 1280x720 front buffer scaled
to the 1080p output (log: `results/2026-10-10-pacing-720p/xsdk.log`). All 8 checks passed.

- **36093 frames, 36093 flips over 36094 vblanks: 0 missed, 0 late, 0 frames wrong.** The longest wait for a front
  buffer was 17950 us, as at 640x480 ("Ten minutes of paced frames, milestone 9").
- **The line counter reached 720 30702 us after the switch,** inside the 16.0 to 32.7 ms worked out above.
- **Every frame checked held its bar.** Frame 0 is still left out of the check, as since the 640x480 fix, because its
  background is whatever eDRAM held.

So milestone 9's pacing holds at the 720p default.

## Shader constants: partial writes hang the GPU, libxenon's whole blocks don't (2026-10-10)

`build/xsdk_test.xex` ran milestone 11's `test_constants` twice. Each time it set three float constants per stage,
one boolean word and one loop constant per draw with the first `xsdk_gpu_set_*_constants`, and each time a fence never
landed and the GPU was kept. The user sent the end of each log (`results/2026-10-10-milestone-11/`). The second
showed the SDK's constant split read back as set and no checked frame wrong, but not which frame hung.

`tests/probe_constants.c` then stepped through the features one draw at a time, each with its own fence (log:
`results/2026-10-10-probe-constants/xsdk.log`, with the whole paste in `paste.txt`).

- **XAM leaves the constant split the SDK sets:** `SQ_VS_CONST` 0x000FF000 and `SQ_PS_CONST` 0x000FF100, vertex
  shaders' 256 constants from 0 and pixel shaders' from 256, as libxenon's setters assume (xe.c:2013-2029).
- **Every shader feature works on constants written libxenon's way.** That means aligned whole blocks: 16 float
  constants at a time, and all 40 boolean and loop dwords at once (xe.c:1217-1221, 1320-1369). The vertex shader read
  c2 and placed a quad with `mad oPos, r1, c0, c1`, two constants in one instruction. The pixel shader read c0.
  `cexec b128` skipped its block with the boolean clear and ran it with it set. `loop i0` ran 0, 1 and 3 passes as
  loop constant 0 said. Each centre held the expected colour. test_constants's six quads, every kind of constant set
  per draw, drew right for 30 frames.
- **Writing only what changed hung on the first frame,** still aligned. That was three float constants per stage, a
  lone boolean word and a lone loop dword. After the hang the command processor had read everything (read pointer =
  write pointer = 0x1016) and `RBBM_STATUS` read busy, so the hang is in the pipeline, not in fetching the packets.
- **Which of the partial writes hangs isn't shown,** the float constants or the lone boolean or loop word, nor whether
  whole blocks need the alignment. The SDK now writes exactly as the passing step did: it keeps a copy of every
  constant, rewrites each 16-constant block a change touches, and rewrites all 40 boolean and loop dwords for any
  change, each packet aligned as libxenon aligns it.
- **The boolean and loop results are the first hardware proof** of those xost encodings: its feature notes list
  boolean constants as unproven (xenos-toolchain/docs/feature-tests.md).

## Shader constants, milestone 11 (2026-10-10)

`build/xsdk_test.xex` ran with the SDK writing constants in whole aligned blocks (log:
`results/2026-10-10-milestone-11-2/xsdk.log`, with the xbWatson lines from the launch and the user's capture). All 124
checks passed, and the console went back to Aurora.

- **test_constants passed:** 6 quads in each of 60 frames, each placed and coloured only by its constants. Every
  checked point was right in all 59 checked frames: the centre and 4 points inside each quad, and 2 outside.
- **Each quad's centre in the capture matches its colour** within a few units of the capture's conversion: BA0201 for
  C00000, 02BE01 for 00C000, 42FD03 for 40FF00 (the boolean), 3F3F3F for 404040 (one loop pass), 0001B9 for 0000C0
  (three), FA817F for FF8080 (all of them). The background reads 202020.
- **The vertex/pixel constant split read XAM's 0x000FF000 and 0x000FF100** before the test and the same after.

With that, milestone 11 is done. Titles set float, boolean and loop constants per draw. Each kind drives a quad on
the console, read back by pixel and seen on a capture, and the test fails when any kind is written wrong.

## Indexed draws and topologies, milestone 12 (2026-10-10)

`build/xsdk_test.xex` ran with `test_indexed`, which draws one rectangle eight ways from index buffers (log:
`results/2026-10-10-milestone-12/xsdk.log`, with the user's screenshot of the last frame at 1280x720). All 133 checks
passed, and the console went back to Aurora.

- **Every way drew its shape in its cell in all 59 checked frames:**
  - triangle lists and strips on 16- and 32-bit indices, filled to their edges and not past them;
  - the outline as a line list on 16-bit indices and as a line strip on 32-bit, each edge found within 2 pixels and
    the inside clear;
  - the corners and centre as a point list, each point within 2 pixels and nothing between them;
  - a triangle list on 32-bit indices past 65535.
- **The vertices were stored out of order,** so each shape needed its indices followed. The past-65535 list's
  16-bit truncation would have landed on degenerate vertices and drawn nothing, so the console reads all 32 bits.
- **The screenshot shows each shape:** the filled rectangles, single-pixel outlines with clear insides, and white
  single-pixel points at the corners and centre of the seventh cell. Its colours are further from the targets than
  earlier captures (CC3141 for E04040, EC7A06 for FF8000), which the log's exact pixel checks rule out as a drawing
  fault.
- **Indices go to the GPU as libxenon sends them** (xe.c:191-200). The size is in 16-bit words with an 8-in-16 swap,
  which 32-bit indices shift to 8-in-32. The vertex cache is invalidated over the indices first.

With that, milestone 12 is done.

## Depth and stencil, milestone 13 (2026-10-10)

`build/xsdk_test.xex` ran with `test_depth` (log: `results/2026-10-10-milestone-13/xsdk.log`, with the user's
screenshot of the last depth-tested frame). All 143 checks passed, and the console went back to Aurora.

- **With the depth test on, all 59 checked frames were right.** Each resolve cleared depth to 0.5 and stencil to 0.
  - **Back to front:** the near rectangle covered the far one.
  - **Front to back:** the far rectangle stayed out of the near one's area and showed only beyond it.
  - **Stencil:** a rectangle tested for stencil equal to 1 drew only where a mask had replaced the stencil with 1.
  - **The cleared depth:** a rectangle at 0.75 stayed hidden behind it, and one at 0.25 showed.
- **With the depth test off, all 9 checked frames got exactly the two depth checks wrong:** the far rectangle in the
  front-to-back overlap, and the one behind the cleared depth. Every other check stayed right.
- **The screenshot shows the same at each check point,** with the colour shift of the last capture (3D33D0 for 4040E0
  in the front-to-back overlap). The far rectangle is cut where the near one is, magenta fills only the mask, and the
  rectangle behind the cleared depth doesn't show.
- **Setting the depth and stencil on their own works.** The SDK writes `RB_DEPTHCONTROL` and `RB_STENCILREFMASK` per
  draw by themselves, where libxenon writes `RB_DEPTHCONTROL` within its 9-register control block (xe.c:1372-1376),
  and the GPU took them.

With that, milestone 13 is done.

## Raster and blend state, milestone 14 (2026-10-10)

`build/xsdk_test.xex` ran with `test_raster` (log: `results/2026-10-10-milestone-14/xsdk.log`, with the user's
screenshot of the last frame). All 152 checks passed, and the console went back to Aurora.

- **All 59 checked frames held all 24 points.** Each point was worked out on the CPU, the blends within 2 per channel.
  - **Blends over a base:** alpha (half-transparent orange over blue, 80807F), additive with green clamped (C0FFD0),
    source less destination with green going negative and clamping to 0 (A00060), and max (C0D090).
  - **The write mask:** only the red and blue of a magenta overlay landed on green, giving E0E0E0.
  - **Culling, with front faces clockwise:** culling back drew the clockwise rectangle and dropped the mirrored one,
    and culling front did the reverse.
  - **The scissor:** its top-left and bottom-right pixels drew, and the pixel just left of it and the one just below
    it didn't. The bottom-right corner is exclusive, at least in y; no point checks just right of it.
  - **The viewport:** a draw over all of clip space filled the viewport's rectangle and nothing beside or below it.
  - **Depth bias:** over a base at depth 0.5 under a less-than test, an unbiased rectangle at the same depth stayed
    hidden, and the same rectangle biased by -0.01 showed.
- **The depth clear of 1.0 holds.** The base in the depth bias cell drew, which a depth clear of 0 would have stopped;
  that was the float rounding fixed before the run.
- **The screenshot shows the same at each check point.** The greys and the lighter blends land within a few steps of
  their targets (80807D, C2FFCF, BFD592, E0E0E0, 7E7E7E). Saturated colours shift as in earlier captures (CB3040 for
  E04040, 8D0059 for A00060, EC7A06 for FF8000). The screenshot's edges are only good to a few pixels, after the
  scaler and the crop. The log's checks pin the scissor's edges to the pixel, and the viewport's to within 3 pixels.
- **Setting blend and raster state on their own works.** The SDK writes `RB_BLENDCONTROL0`, `RB_COLOR_MASK`,
  `PA_SU_SC_MODE_CNTL` and the four `PA_SU_POLY_OFFSET` registers per draw, and the scissor and viewport between
  draws. libxenon packs the blend and cull fields into its control block (xe.c:1451-1455, 1505-1511). The GPU took the
  SDK's separate writes mid-frame.
- **The slope term of depth bias isn't tested.** The API takes it, but this run only used the constant term.

With that, milestone 14 is done.

## A small texture's mips share its base's tail (2026-10-10)

Milestone 15's first run of `build/xsdk_test.xex` passed 163 of 164 checks (log tail:
`results/2026-10-10-milestone-15/xsdk-run-1-tail.log`). Only the 16x16 tiled texture failed, and only its levels 1 to
4. Its base, read from inside the packed tail, was right. Every pixel of the four levels held an earlier test's clear
colour, read as a texel. GPU memory isn't cleared when allocated, so the GPU had read texels the test never wrote.

`build/probe_mip_tail.xex` settled where those levels are (log: `results/2026-10-10-probe-mip-tail/xsdk.log`).

- **The setup:** six tiled textures whose short side is 16 or less (16x16, 8x8, 4x4, 16x8, 8x16 and 32x16), drawn one
  texel to a pixel at every level. Every texel of four pages around each texture held a code for its page and place.
- **With the mip address on the page after the base, every level came from the base's page.** None came from the mip
  page or from outside the four pages.
- **With the mip address set to the base, every level came from the same texels as before.** So the console ignores the
  mip address once the base is in the tail.
- **Every level read one rectangle at Xenia's tail offsets** (xenia:src/xenia/gpu/texture_util.cc:116-207). That
  includes the base at (16, 0) for a square and (0, 16) for a wide texture. It matches Xenia's own run of the probe,
  page aside.
- **Xenia is wrong to give these mips a tail of their own at the mip address**
  (xenia:src/xenia/gpu/texture_util.h:85-98). Xenia reads from the mip address, so the probe's first setup would pass
  in Xenia and fail on the console.

The SDK now keeps such a texture's mips in its base's tail, and points the mip address at the base so Xenia reads them
there too. `test_sampling` adds a 16x8 texture to the 16x16 one, and 32x32 and 24x20 textures, whose tail starts at
level 1, the case Xenia's note sets apart next (texture_util.h:91-94).

## Sampling and mipmaps, milestone 15 (2026-10-10)

The second run of `build/xsdk_test.xex` ran with `test_sampling` and the small-texture fix (log:
`results/2026-10-10-milestone-15-2/xsdk.log`, with the user's screenshot of the last frame). All 164 checks passed,
and the console went back to Aurora.

- **All 59 checked frames held all 560 points, and every pixel of the last frame was right:** 221,775 pixels across 56
  cells and the ring of clear pixels around each.
- **Mip levels, drawn one texel to a pixel, came from where the SDK put them:**
  - **128x128:** the packed tail from level 3.
  - **136x68 and 68x136:** each level's rows and pitch sized as if the base were 256x128 or 128x256, and the tail laid
    out wide or tall.
  - **32x32 and 24x20:** the tail from level 1.
  - **16x16 and 16x8:** the base and its mips in one tail.
  - **16x16 linear, with one level:** its base in the tail at (16, 0) with a 64-texel pitch.
- **The GPU picked each level from the drawn size.** The non-square textures' levels came to up to 0.32 above a whole
  level, and still read that level. Nothing here tells rounding to the nearer level from rounding down.
- **Filters, each with the other filter of the pair set the other way:**
  - **Magnified four times:** point matched exactly; linear stayed within 3 per channel of a bilinear blend around texel
    centres.
  - **Minified twice, a quarter texel in:** point matched exactly; linear stayed within 3 of weights of 1/4 and 3/4.
  - **So the GPU took the magnification filter below level 0 and the minification filter above it.**
- **Mip filters, at level 1 biased by the fetch constant's 12/32:** point read level 1 alone, and linear blended 3/8 of
  level 2 into it, within 3.
- **Addressing, with half the texture past each edge:** wrap, mirror and clamp were right, and so was mirroring across
  with clamping down. So the u and v modes sit where the SDK writes them.
- **The screenshot shows each level's hue** where a level is big enough to sample. The 1- and 2-pixel levels blur in
  the screenshot after the scaler; the log checks them.
- **Not covered:**
  - **Anisotropic filtering:** it waits, as SCOPE.md says.
  - **The border and mirror-once modes:** not offered.
  - **Linear textures with mips:** creating one fails with `STATUS_NOT_IMPLEMENTED`.
  - **A level of detail exactly halfway between two levels:** avoided.
  - **The precision of linear filtering:** only bounded by the tolerance of 3.

With that, milestone 15 is done.

## Texture formats, milestone 16 (2026-10-11)

`build/xsdk_test.xex` ran with `test_formats` (log: `results/2026-10-11-milestone-16/xsdk.log`, with the user's
screenshot of the last frame). All 175 checks passed, and the console went back to Aurora.

- **All 59 checked frames held all 620 points, and every pixel of the last frame was within tolerance:** 123,102 pixels
  across 62 cells and the ring of clear pixels around each.
- **Every format read back right, linear and tiled:** A8R8G8B8, R5G6B5, A1R5G5B5, A4R4G4B4, A2B10G10R10, R8, R8G8,
  R16F, R16G16F, R16G16B16A16F, DXT1, DXT3 and DXT5.
- **The fixed-point and half-float formats matched the CPU's decode exactly,** alpha included: the largest error in any
  channel was 0.
  - The 4-, 5-, 6- and 10-bit channels and the 1- and 2-bit alphas came out as the value times 255 over its maximum,
    rounded.
  - Halves of 2.0 and -1.0 clamped to 255 and 0 in the 8-bit target.
- **So each format's endian swap and component order are right.**
  - **Endian swaps:** 8-in-16 for 16-bit units, half floats and DXT's words; 8-in-32 for 32-bit units; none for bytes.
  - **Component order:** x is the lowest bits or the first byte, as Xenia maps the formats to Vulkan.
- **DXT decoded as the CPU worked it out:**
  - **Endpoints and DXT1's transparent black came out exact,** within the tolerance of 1.
  - **Blends came within 3 (DXT1, DXT3) and 4 (DXT5)** of exact thirds, halves, sevenths and fifths. Xenia's host
    GPU was off by up to 6.
  - DXT1's three-colour mode with transparent black works, and so do DXT5's six-alpha mode with 0 and 255, and DXT3's
    4-bit alphas.
- **Mips work in blocks:** every level of 32x32 tiled R8, R5G6B5, A2B10G10R10, R16G16B16A16F, DXT1 and DXT5 textures
  read right. That covers 1-, 2-, 4- and 8-byte texels, and 8- and 16-byte blocks, each with its tail from level 1 and
  DXT's tail offsets in blocks.
- **The screenshot's linear and tiled rows match each other,** within the screenshot's own scaling noise: at most 10 in
  a channel, 2.2 on average.
- **Not covered:**
  - DXT3 and DXT5 colour blocks with the first endpoint not above the second: the test kept them four-colour.
  - Signed and gamma textures.
  - The formats outside the milestone, such as DXN, 16-bit fixed point and 32-bit float.
  - Linear filtering of these formats: the test point-sampled.
  - Whether linear rows need 256-byte alignment: the SDK always uses it, so nothing tested less.

With that, milestone 16 is done.

## Vertex input, milestone 17 (2026-10-11)

`build/xsdk_test.xex` ran with `test_vertex` (log: `results/2026-10-11-milestone-17/xsdk.log`, with the user's
screenshot of the last frame). All 183 checks passed, and the console went back to Aurora.

- **All 59 checked frames held all 115 points, and every pixel of the last frame was right:** 100,188 pixels across 23
  cells and the ring of clear pixels around each.
- **Every vertex format read right as a colour,** each square within 1 of the CPU's decode, alpha included:
  - float1 to float4;
  - half2 and half4;
  - byte4, short2 and short4, unsigned and signed, normalised and whole numbers.
- **Sign and scale came out right.**
  - Each signed format's negative clamped to 0 in the 8-bit target.
  - Each whole-number format's 2 clamped to 255, where a normalised read would have given nearly 0.
  - So the fetch's sign and number-format bits sit where the SDK writes them. This is the first hardware result for
    byte vertex formats, which xost lists as unproven (xost:README.md:89-91).
- **Components a format lacks read 0, and w read 1, as libxenon patches them** (xe.c:900-916).
  - float1, float2, float3, half2 and the short2 formats drew alpha 255 as colours.
  - Positions in half2, short2n, float2 and float3 drew in place, which needs w to be 1.
- **Streams 0 to 6 and 15 worked, each fetch reading the constant 95 minus its stream.** They span fetch constant
  groups 31, 30, 29 and 26.
  - Positions came from streams 0, 3 or 15 and colours from 1, 2 or 4, with strides from 4 to 28 bytes and offsets of
    0 or 4.
  - Writing a stream sent its whole group from a kept copy, and the other streams in that group stayed intact.
  - One cell read position and colour from a single stream, 24 bytes a vertex.
- **The screenshot shows each cell's four squares,** the formats without blue and alpha in reds and greens.

With that, milestone 17 is done.

## Recorded command buffers, milestone 18 (2026-10-11)

`build/xsdk_test.xex` ran with `test_replay` (log: `results/2026-10-11-milestone-18/xsdk.log`). All 193 checks
passed, and the console went back to Aurora.

- **The GPU ran packets from memory with `PM4_INDIRECT_BUFFER`,** two dwords after the header: the buffer's GPU
  address, then its length in dwords, as Xenia reads them (xenia:src/xenia/gpu/command_processor.cc:943-953).
- **The recordings:**
  - **A background of 86 dwords:** the target, the shaders, stream 0 and a draw of four squares, recorded once.
  - **A bar of 6 dwords:** one draw, recorded once.
- **Each of 120 frames ran the background once and the bar twice.** Before each bar, the ring pointed stream 0 at a
  different moving bar.
  - So one buffer ran more than once in a frame, and several ran in a frame.
  - State set on the ring between runs carried into the recorded draws.
- **Every frame came back with both bars where its frame put them, and all four squares.** None of the reused buffers
  held the wrong frame.
- **Milestone 9's checks held through the present path,** with 2 buffers flipping every vblank. The 120 frames took
  120 flips over 120 vblanks, with none missed and none late, and the longest wait was 17,948 us.
- **The recordings' padded constant writes still aligned,** as the buffers are page-aligned like the ring.
- **A recording that outgrew its 4-dword buffer** failed with `STATUS_BUFFER_TOO_SMALL` and left the buffer empty.
- **Not covered:**
  - running a buffer from inside another;
  - `PM4_INDIRECT_BUFFER_PFD`;
  - buffers large enough to take long to run.

With that, milestone 18 is done, and so are all of milestones 11 to 18.

## The splash stays up until present's first frame (2026-10-11)

Two runs, at `7420d94` (logs: `results/2026-10-11-splash-handover/`):
- **vulkan4xenon's milestone 1 title** (`vulkan4xenon_test.log`). It shows the splash with `xsdk_splash_begin` from
  `vkCreateDevice`, and its swapchain is `xsdk_present`. All 20 checks passed, and the console went back to Aurora.
- **`build/xsdk_test.xex`** (`xsdk_test-tail.log`, the tail the user sent). All 194 checks passed, the splash's
  among them: `xsdk_splash_show` is now begin, a hold and end, and this was its first run on the console.

What the first run showed:
- **`xsdk_splash_begin` put the logo on screen.** `D1GRPH_PRIMARY_SURFACE_ADDRESS` went from XAM's 640x480 surface at
  0 (see "At title start the display shows a 640x480 surface at address 0") to the splash's buffer at 0x1F4F0000.
- **`xsdk_present_open` took the display over without showing its own black buffer.** Afterwards the display still
  showed 0x1F4F0000, which isn't one of present's buffers.
- **The first present flipped to buffer 0** (0x1F150000), whose centre read the app's clear colour exactly (00 60
  00).
- **The present chain kept going afterwards.** The splash's buffers are freed a vblank after the first flip; that
  step isn't logged. All 120 later frames were shown: 121 flips over 132 vblanks, none late. The 10 vblanks missed
  came from the title, which waits for each frame and reads it back before presenting it. The logo's frame was freed
  only after it had left the screen, and nothing hung.
- **`D1GRPH_PRIMARY_SURFACE_ADDRESS` reads back the buffer on screen** (`xsdk_display_shown_address`): each read
  matched the buffer the test expected.
- **On screen,** the user reported for the first run: the logo faded in, held for a second or two, the app's dark
  green flashed, then its triangle showed. The titles check which buffer is shown, not what's in it.
- **Not covered:** `xsdk_splash_end` with the splash still up, which a device closed before any swapchain would take.

## Textures in memory the caller owns, and byte-order RGBA (2026-10-11)

Two runs, at `7ff6c4c` (logs: `results/2026-10-11-texture-attach/`):
- **`build/xsdk_test.xex`** (`xsdk_test-tail.log`, the tail the user sent). All 194 checks passed.
  - `xsdk_texture_create`, now `xsdk_texture_init`, an allocation and `xsdk_texture_attach`, made every texture the
    earlier milestones' tests sample.
  - `test_formats` passed with its 66 cells, which include `XSDK_FORMAT_R8G8B8A8` and `XSDK_FORMAT_B8G8R8A8`, linear
    and tiled.
- **vulkan4xenon's milestone 2 title** (`vulkan4xenon_test.log`). All 33 checks passed. Its images are
  `xsdk_texture_init`, then `xsdk_texture_attach` at an offset into memory the app allocated. The two byte-order
  formats showed every texel of a 4x4 image exactly: red and blue in place, each format's bytes unswapped
  (k_8_8_8_8, `ENDIAN_NONE`).

## Triangle fans, and raster facts through vulkan4xenon (2026-10-11)

vulkan4xenon's milestone 3 title ran at xsdk `604bdfb` (log and two captures: `results/2026-10-11-fans-and-raster/`).
All 45 checks passed, and the console went back to Aurora. It drives xsdk's state blocks through Vulkan. Along the
way it settled what xsdk's own tests hadn't:

- **Triangle fans draw.** `XSDK_PRIMITIVE_TRIANGLE_FAN` (Xenos's `kTriangleFan`, xenia:src/xenia/gpu/xenos.h:50), on
  16-bit indices: four indices from the top left filled the quad to its edges.
- **Facing is decided after the viewport.** vulkan4xenon's viewport has a positive y scale, the reverse of xsdk's.
  Culling still followed winding as the triangle looks on screen, clockwise with `front_clockwise` set.
- **Pixel centres sit on whole coordinates under `PIX_CENTER_D3D`.** With the viewport moved half a pixel up and
  left, edges at x = 600.25 and 700.25 covered columns 600 to 699, and the same in y. That's what centres on halves
  give for Vulkan's coordinates, and what Xenia says of `kD3DZero` (xenia:src/xenia/gpu/xenos.h:861-866).
- **The scissor's right edge is exclusive too.** Milestone 14 showed the bottom edge only. Here, a 60 by 50 scissor at
  (280, 510) drew pixel (339, 559), but not (340, 535) or (310, 560).
- **Reverse subtract and min blend** as their names say, within 2 per channel: destination less source, and the
  smaller of each. Milestone 14 had covered alpha, additive, subtract and max.
- **A colour write mask of 0 with stencil replace** wrote the stencil and no colour.
- **Depth bias in depth units:** -0.01 pulled a quad at 0.3 in front of an equal-depth one under less-than, as in
  milestone 14. Here it was given as -0.01 x 2^24 in Vulkan's units and divided back.

## Placed targets, resolves that clear less, and byte-order resolves, through vulkan4xenon (2026-10-11)

Two runs, at `c5d87ed` (logs and a capture: `results/2026-10-11-render-to-texture/`):
- **`build/xsdk_test.xex`** (`xsdk_test.log`). All 194 checks passed, and the console went back to Aurora, so the
  existing callers lost nothing: `xsdk_gpu_set_target` and the resolves still write what they did.
- **vulkan4xenon's milestone 4 title** (`vulkan4xenon_test.log`, `render_to_texture.png`). All 58 checks passed, and
  the console went back to Aurora.

That title's eight render passes a frame, for 60 frames, ran on the xsdk additions made for them:

- **Targets placed with `xsdk_gpu_set_target_at`:** colour from tile 0 and depth from 2048 less the target's tiles,
  for a 200x120 target with depth, colour-only targets of 96x64 and 64x32, and the 1280x720 swapchain. Depth tested
  right at the top of eDRAM.
- **A resolve that clears neither leaves eDRAM as it was** (`xsdk_gpu_set_resolve_clears(false, false)`):
  - depth survived a colour-only target's draws and resolve between two passes: a quad at depth 0.45 stayed hidden
    behind one at 0.3 drawn two passes before;
  - colour survived for the next pass, which drew over it without drawing it back.
- **Byte-order resolves** (`xsdk_gpu_resolve_texture`): R8G8B8A8 without the red-blue swap and B8G8R8A8 with it. Read
  back from memory and sampled, red and blue were in place, within 2 per channel.
- **Resolving the pitch:** a 200-wide texture resolved across its 224-texel pitch, and its column 199 held what was
  drawn there.
- **Sampling straight after a resolve** into the same texture, in the same submission, as milestone 8 did, now for
  the byte-order formats. A front buffer also sampled as a tiled B8G8R8A8 texture of its size.
- **Xenia differs:** in vulkan4xenon's title, a draw sampling an image resolved into earlier in the run got its
  earlier contents, even across submissions, though `test_render` passes there with an R8G8B8A8 target too. The
  console is right; the cause in Xenia isn't known.

## Sleeps, write speeds, and a set-up that wouldn't run from a cached stream (2026-10-11)

vulkan4xenon's vkbench, at xsdk `8859829` (log: `results/2026-10-11-milestone-19-stream/vkbench.log`), for milestone
19. Its probes ran before the GPU opened:

- **A 1 ms sleep took 1993 us on average,** from 1839 to 1999 over 100 (`xsdk_thread_sleep(1)`, so
  `KeDelayExecutionThread`). So the ring's wait for room, which slept 1 ms at a time, cost about 2 ms a wait.
- **Writing 1 MB, fastest of 8:**
  - into write-combined memory with volatile 32-bit stores: 291 us, 3603 MB/s;
  - into write-combined memory with newlib's `memcpy`: 5658 us, 185 MB/s, 20 times slower;
  - into cached memory with 32-bit stores, 667 us, then `dcbst` over it, 94 us: 1377 MB/s in all.
- **The set-up never finished from a cached stream.** At that commit every packet, `xsdk_gpu_set_up`'s included,
  went into 4 MB of cached memory, written out with `dcbst` and run from the ring with `PM4_INDIRECT_BUFFER` after a
  vertex cache invalidate, as libxenon runs its own (libxenon:drivers/xenos/xe.c:52-75, 83-147). The set-up's fence
  didn't land within 1 s, so `xsdk_gpu_open` returned `STATUS_IO_TIMEOUT`. Whether the console refused the cached
  memory or the set-up's packets in an indirect buffer isn't isolated. libxenon loads its own microcode (xe.c:224-252,
  336-343); the SDK runs the kernel's.

So the set-up goes straight into the ring again, as before, and the stream moved to write-combined memory, where
milestone 18's recorded buffers ran and 32-bit stores were faster.

## A write-combined stream ran its first segment and hung on the next (2026-10-11)

vkbench again, at xsdk `f1d385c` (log: `results/2026-10-11-milestone-19-stream-2/vkbench.log`, the whole of it; the
title hung on the boot logo). The set-up went straight into the ring and finished, and later packets went into a
write-combined stream.

- **The probes repeated:** a 1 ms sleep took 1990 us on average; write-combined 32-bit stores again ran at 3603 MB/s,
  `memcpy` at 185 MB/s, and cached stores with `dcbst` at 1413 MB/s.
- **The stream's first segment ran:** the xsdk backend's start waits for a resolve and a fence written into the stream
  (`clear_first`), and it passed. That segment started at the stream's offset 0.
- **The first frame never finished.** Its segments started wherever the one before ended, at any dword. libxenon starts
  each of its segments on a 128-byte line (libxenon:drivers/xenos/xe.c:123), and milestone 18's buffers were
  page-aligned. So the stream now starts each segment on a 128-byte line; the next run tells whether that was all.

## Segments on 128-byte lines run, and the waiting is gone (2026-10-11)

vkbench at xsdk `b5933a0` (log and CSVs: `results/2026-10-11-milestone-19-stream-3/`). All 14 checks passed, both
backends drew the same picture in every scene, and the console went back to Aurora. The only change from the run that
hung was each stream segment starting on a 128-byte line, so that was the cause.

- **No waits:** in every scene, the ring and the stream never made the CPU wait. `draws_4000` ran 602.6 segments a
  frame, 156.4 dwords a cube.
- **Draw and submit, median, against the first run** (vulkan4xenon:results/2026-10-11-milestone-5/vkbench_run1.log):
  - xsdk `draws_1000` 1,213 us, from 37,590, so 1.21 us a cube; `draws_4000` 4,836 us, from 151,190; `states_1000`
    2,167 us, from 55,496;
  - vulkan4xenon `draws_1000` 2,867 us, from 47,515; `draws_4000` 11,703 us, from 195,871.
- **The GPU still finished 0.3 ms after the submit** in the cube scenes, and the GPU-bound scenes kept their times.
- **Reading uniform buffers is now the largest cost through vulkan4xenon:** `uniforms_1000` took 6,852 us against
  `draws_1000`'s 2,867, 4.0 us a draw for its two blocks read from mapped write-combined memory at submit. The first
  run measured 1.0 us.

## Every test passes on the stream (2026-10-11)

Two runs at xsdk `5a06bc3` (the tails the user sent: `results/2026-10-11-milestone-19-regression/`):
- **`build/xsdk_test.xex`:** all 197 checks passed, the 194 from before and the three for the counters and the
  stream, and the title exited with status 0. So every drawing test, milestone 18's recorded buffers among them, ran
  from the stream.
- **vulkan4xenon's test title:** all 58 checks passed, and the title exited with status 0.

With the run before, milestone 19's done-when holds on the console: `draws_1000` drew and submitted in 1,213 us,
under 16 ms; `draws_4000` is recorded with its counts; both titles pass in full; and vkbench's backends drew the same
pictures. Its third step, fewer dwords a draw, hasn't been started.

## Copies into write-combined and cached memory (2026-10-11)

vkbench at vulkan4xenon `86866e8`, xsdk `a465595` (log: `results/2026-10-11-milestone-20-probe/vkbench.log`), for
milestone 20. All 14 checks passed again, with draw and submit times within 4% of the run before. Its probes copied
1 MB from memory, flushing the source from the cache with `dcbf` before each of 8 rounds, and every copy came out
right:

- **Into write-combined memory:** newlib's `memcpy` 185 MB/s; 32-bit stores with `dcbt` 517 MB/s; 64-bit stores with
  `dcbt` 883 MB/s; VMX stores 568 MB/s; VMX stores with `dcbt` 8 lines ahead 1485 MB/s.
- **Into cached memory, flushed too:** newlib's `memcpy` 410 MB/s; VMX with `dcbt` 1823 MB/s; VMX, `dcbt` and `dcbz`
  2274 MB/s.
- So aligned 64-bit and VMX stores work in write-combined memory, and VMX with `dcbt` copied fastest into it.
- **Reading the source from memory bounds a copy:** in-order 32-bit stores of values the CPU made wrote 3603 MB/s,
  where copying with the same stores ran at 517 MB/s.
- **VMX runs in titles,** as it already did in the C++ unwinder, which saves vector registers with `stvx` when a test
  throws.

## The SDK's memcpy, milestone 20 (2026-10-11)

Three runs at xsdk `bf425b9` and vulkan4xenon `109487a` (logs: `results/2026-10-11-milestone-20-memcpy/`, with the
tails the user sent for the two test titles):
- **vkbench** (`vkbench.log`): all 14 checks passed, both backends drawing the same picture in every scene. Every copy
  in the title went through the SDK's `memcpy` (`src/copy.c`), and its probes timed it:
  - into write-combined memory: 5577 MB/s for 256 KB from the cache, and 1476 MB/s for 1 MB from memory, where
    newlib's managed 185 MB/s;
  - into cached memory: 1814 MB/s for 1 MB from memory, where newlib's managed 410 MB/s;
  - prefetching 16 lines ahead copied at 1508 MB/s and 32 lines at 1315 MB/s, against 1470 MB/s for 8, so `memcpy`
    stays at 8;
  - `stream_8000` drew and submitted in 526 us through xsdk, from 5,553, and in 539 us through vulkan4xenon, from
    5,562; `stream_1000` in 32 us, from 698. The other scenes stayed within 3%.
- **`build/xsdk_test.xex`** (`xsdk_test-tail.log`): all 201 checks passed and the title exited with status 0. Among
  them were the four copy checks: every size from 0 to 300 at every source and destination alignment, 1 MB at four
  alignments, copies into write-combined memory, and `memmove` over itself both ways.
- **vulkan4xenon's test title** (`vulkan4xenon_test-tail.log`): all 58 checks passed and it exited with status 0.

With that, milestone 20 is done: `memcpy` writes write-combined memory at 5.6 GB/s from the cache, `stream_8000` fell
to match, and every test passes.

## Float constants go in groups of 4 (2026-10-11)

`build/probe_constants.xex`, with the steps added at `af68195` (log:
`results/2026-10-11-milestone-19-constant-groups/xsdk.log`), for milestone 19's third step. Its first 11 steps
passed as before. Then test_constants's six quads, each stage's c0 to c2 written per draw and the integers kept one
whole block, for 30 frames each:

- **In groups of 4, from c0:** every frame landed and every centre was right, as aligned type-0 packets and through
  `SET_CONSTANT` with its values aligned the same way.
- **One constant at a time** hung on the first frame, each packet aligned. The command processor had read everything
  (read pointer = write pointer = 0x1017) and `RBBM_STATUS` read busy, as in milestone 11's hang. So the lone float
  constants, at least, hung that run; whether its lone boolean and loop words would too isn't shown.
- **One at a time through `SET_CONSTANT`, and groups of 3,** didn't run after the hang.

So the SDK writes float constants in groups of 4, from a multiple of 4, rather than libxenon's 16.

## Fewer dwords a draw, milestone 19 (2026-10-11)

Three runs at xsdk `63fd069` and vulkan4xenon `bf60e98` (logs: `results/2026-10-11-milestone-19-step-3/`, with the
tails the user sent for the two test titles). Float constants went in groups of 4, and titles invalidated the GPU's
caches after writing memory, not every draw.

- **`build/xsdk_test.xex`** (`xsdk_test-tail.log`): all 201 checks passed, and the title exited with status 0. Among
  them are the tests that reuse memory for new vertices, the pacing modes and the recorded buffers' moving bars, which
  showed stale vertices when the vertex cache wasn't invalidated (see "The splash stays up until present's first
  frame"). So every write the tests make is invalidated where it needs to be.
- **vulkan4xenon's test title** (`vulkan4xenon_test-tail.log`): all 58 checks passed, and it exited with status 0,
  invalidating all memory once a submit.
- **vkbench** (`vkbench.log` and its CSVs): all 14 checks passed, both backends drawing the same picture in every
  scene, with no waits.
  - A cube draw wrote 49.1 dwords through xsdk, from 156.4, and 87.1 through vulkan4xenon, from 200.3.
  - Drawing and submitting `draws_1000` took 626 us through xsdk, from 1,210, so 0.63 us a cube; `draws_4000` took
    2,487 us, from 4,823; `states_1000` 1,022 us, from 2,145.
  - Through vulkan4xenon, `draws_1000` took 1,503 us, from 2,878; `draws_4000` 6,218 us, from 11,752;
    `uniforms_1000` 5,309 us, 3.8 us a draw more than `draws_1000` for reading its uniform buffers.
  - In xsdk's `states` scenes the GPU now finished 0.8 ms after the submit, from 0.3: the CPU writes a texture and
    shader change faster than the GPU runs it. The frame's draw, submit and GPU time still fell, 1.8 ms from 2.4.

With that, milestone 19 is done. `draws_1000` draws and submits in 626 us through the GPU layer, 0.63 us a cube, where
the milestone asked for under 16 ms and started at 37,590 us; `draws_4000` is recorded with its counts; and every test
passes.

## Constants loaded from memory go in groups of 4 too (2026-10-11)

`build/probe_constants.xex` at `d3d50a5` (log: `results/2026-10-11-constant-loads/xsdk.log`), for vulkan4xenon's
milestone 6. Its first 13 steps passed as before; then test_constants's six quads, each stage's c0 to c2 loaded with
`PM4_LOAD_ALU_CONSTANT` from the quad's own write-combined memory (address, then type 0 and the index, then the
dwords), for 30 frames each:

- **In groups of 4, from c0:** every frame landed and every centre was right. So the command processor loads float
  constants from memory, big-endian words as the CPU wrote them.
- **In groups of 3** hung on the first frame, read pointer = write pointer = 0x1017 and `RBBM_STATUS` busy, as one
  constant at a time did written as registers.
- **One at a time through `LOAD_ALU_CONSTANT` or `SET_CONSTANT`, and groups of 3 written as registers,** didn't run.

So every route tried writes float constants in whole groups of 4: type-0 packets, `SET_CONSTANT` and
`LOAD_ALU_CONSTANT`.

## Constants loaded from memory, for titles and vulkan4xenon (2026-10-11)

Runs at xsdk `5d4bda6`, with xost at `b9b38e9` (logs: `results/2026-10-11-constant-loading/` and
vulkan4xenon:results/2026-10-11-milestone-7/):
- **`build/xsdk_test.xex`** (`xsdk_test-tail.log`): all 203 checks passed, and the title exited with status 0. Among
  them, test_constants drew its six quads for 60 frames with each stage's float constants loaded from memory through
  `xsdk_gpu_load_vertex_constants` and `xsdk_gpu_load_pixel_constants`, a group of 4 from c0 out of a 64-byte slot
  each, and every checked point was right.
- **vulkan4xenon's test title and vkbench** loaded every uniform block with them, at the registers xost now gives
  uniform blocks: whole groups of 4 to each. The title passed all 58 checks and vkbench all 14, both backends drawing
  the same pictures. vkbench's `uniforms_1000` drew and submitted through the layer in 717 us, from 2,091, with no
  uniform data read by the CPU.

So the GPU loads float constants from memory for titles, in whole groups of 4 from a multiple of 4.

## Calls between xsdk's GCC and the console's MSVC (2026-10-11)

A probe title of 36 cases, built once with the user's leave to compile its other half with the Xbox 360 SDK's own
compiler: GCC code called that MSVC code with each case and it called GCC code back, each side recording what
arrived (log: `results/2026-10-11-abi-probe/abi_probe.log`). The MSVC half stays in `local/`, out of git. The kernel
and XAM are MSVC's, so this is how they take calls from a title and make them into one. Xenia gave the same results.

| Case | GCC calls MSVC | MSVC calls GCC |
|---|---|---|
| 8 or fewer int or pointer arguments | pass | pass |
| arguments past the 8th | fail | fail |
| a `uint64_t` argument, or one returned | fail | fail |
| `char` and `short` arguments | pass | fail |
| `char` and `short` returned | pass | pass |
| a `float` and a `double` | pass | pass |
| int, float, int, double, int | fail | fail |
| 9 doubles | fail | fail |
| structs of 1, 8 and 24 bytes and of two floats, by value | fail | fail |
| an 8-byte struct returned | fail | fail |
| a 24-byte struct returned | pass | pass |
| varargs of int, `uint64_t`, double, int | fail | fail |
| r14-r31, f14-f31 and v14-v31 kept across the call | pass | fail |

So the console's code (MSVC 16.00.11886) takes each argument in one register, r3 to r10: a 64-bit integer whole, a
`float` or `double` in f1 to f13 while also taking up an integer register's place, a struct as its bytes across as
many registers as it fills. Structs of 8 and 24 bytes start at a register's high byte; a 1-byte struct sits in the
low byte (`clrlwi r10,r4,24` in the MSVC half's code); the probe has no other sizes. Arguments past the 8th take
8-byte slots from 0x50(r1), a 32-bit value in a slot's low word, so the 9th at 0x54. It returns 64-bit integers and
structs of 8 bytes or less in r3, larger structs through a pointer in r3. Whoever receives a `char` or `short`
extends it: the callee its arguments, the caller what comes back (`extsb r3,r3` after the call). It keeps r14-r31
whole, f14-f31 and v14-v31; it changed CR6.

xsdk's GCC, the 32-bit SysV convention, differs in each failing case: register pairs for 64-bit integers, structs by
pointer, arguments past the 8th 4 bytes apart from 8(r1), doubles in f1 to f8 only, 8-byte structs back in r3 and r4,
callers that extend, and r14-r31 kept by their low halves only, v20-v31 only. In real MSVC code, 3 of 12 `uint64_t`
values kept across a GCC call came back wrong. Layout agreed for doubles, 64-bit integers, padding and a 3:7:22
bitfield, but not `wchar_t` (2 bytes in MSVC, 4 in GCC) or a struct mixing `u8` and `u32` bitfields (12 bytes in
MSVC, 4 in GCC). r2 was 0 and r13 0x801B0000, the processor control region, before and after each side's code.

What xsdk did about it, until milestone 22 had GCC call as MSVC does ("Calls between GCC and MSVC, with GCC calling as
MSVC does"):
- **Imports:** the stubs pass r3 to r10 through and move arguments past the 8th into the kernel's slots, so imports
  take only integers and pointers of 32 bits or less. `gen_imports.py` refuses a declaration with a 64-bit integer,
  floating point or a struct by value, until a stub converts them. An import that takes UTF-16 text takes `uint16_t`,
  not `wchar_t`; none does yet.
- **The kernel calling a title:** `XSDK_KERNEL_ENTRY` saves r14-r31 whole around the title's code, and the toolchain
  keeps GCC off v14-v19 (`-ffixed-v14` to `-ffixed-v19`).

## The kernel's calls into a title keep its registers (2026-10-11)

`build/xsdk_test.xex` at `c806a87`, built with the toolchain that leaves v14-v19 alone (log:
`results/2026-10-11-kernel-entry/xsdk_test.log`): all 210 checks passed, and the title exited with status 0.
- **XSDK_KERNEL_ENTRY:** GCC code that keeps 64-bit values across calls and twenty vectors live, called directly,
  changed the high halves of r27-r31; called through the trampoline, r14-r31 came back whole, and f14-f31 and v14-v31
  unchanged. GCC's code touched none of v14-v19.
- **The audio callback,** which the kernel calls through the trampoline, ran 376 times in 2000 ms and played its tone,
  as before.
- **Threads' stacks:** a thread's stack, read from its processor control region, is 1 MB by default
  (`ExCreateThread` with a size of 0); asked for 384 KB through pthread attributes, it is 393216 bytes.

## The time base runs at 49.875 MHz (2026-10-11)

SDL's check title (`test/xsdk/sdl_check.c` in the SDL fork; logs: `results/2026-10-11-sdl-check/xsdk.log`, then
`results/2026-10-11-sdl-check-2/xsdk.log`) found three things wrong with the SDK's clocks:
- **`CLOCK_MONOTONIC_RAW` was refused.** SDL's timer reads it wherever the C library defines it, and fell back to
  `gettimeofday`, the kernel's system time: `SDL_Delay(100)` took 101000 us and `SDL_DelayPrecise(16 ms)` 16000 us,
  whole milliseconds, and every check passed. The SDK now reads the raw clock from the time base, and the title checks
  that SDL's clock steps finer than 0.1 ms.
- **The time base runs 0.25% slower than the kernel reports.** Read from the time base, the second run's
  `SDL_Delay(100)` took 99774 us. `KeQueryPerformanceFrequency` gives 50000000 Hz
  (`results/2026-10-10-probe-vblank/xsdk.log`), and at that rate the 60 Hz vblank there lasts 16624 us (930 + 15694),
  and vkbench's frames, 20 scenes of 600 paced to it, 16625 to 16627 us
  (vulkan4xenon:results/2026-10-11-milestone-8-reverted/vkbench_summary.csv). 16667 us times 49.875/50 is 16625 us:
  the time base runs at the CPU's 3192 MHz over 64, as libxenon has it (libxenon:libxenon/drivers/ppc/timebase.h:6),
  while the display and the kernel's sleeps keep true time. The SDK's monotonic clocks now count at 49875000 Hz. Times
  logged before read 0.25% short: those 16625 us frames are 16667 us. Xenia runs its time base at the 50 MHz the kernel
  reports (xenia:src/xenia/emulator.cc:151), so there the clocks run 0.25% fast, and the check that they keep the
  kernel's time fails.
- **The kernel's sleep ends early.** At the true rate, the 250 ms `sleep_for` of earlier runs, logged as 248 to 250 ms
  (248 in `results/2026-10-10-milestone-12/xsdk.log`, 249 or 250 in most), took 248.6 to 251.6 ms: a relative
  `KeDelayExecutionThread` can end up to 1.4 ms before the time asked for, which fits a wait counted from the kernel's
  last clock tick. `nanosleep`, and so `sleep_for` and `SDL_Delay`, now waits again until `CLOCK_MONOTONIC` passes its
  deadline.

## SDL's drivers and the fixed clocks on the console (2026-10-11)

`sdl_check.xex` from the SDL fork at `3d6afe95b` and `build/xsdk_test.xex`, both on the SDK at `ac1747b` (logs:
`results/2026-10-11-sdl-check-3/xsdk.log` and `results/2026-10-11-clocks/xsdk_test.log`): all 16 and all 215 checks
passed, and both titles exited with status 0.
- **SDL:** the platform reads "Xbox 360". `game:\` is the base path, and `game:\xsdk\sdl_check\` was made and a file
  written there read back. A thread ran. The controller showed up as an Xbox 360 controller, its buttons and sticks
  read, and both motors rumbled. Audio opened at 6 channels of big-endian floats at 48 kHz and played a 750 Hz tone.
- **The clocks:** `CLOCK_MONOTONIC` kept the kernel's system time to 7 ppm over 252 ms, where it was 2500 ppm off, and
  the system time stepped 1 ms at a time. Twenty 2 ms `nanosleep`s took 2.05 to 3.00 ms, none early, and
  `SDL_Delay(100)` 100016 us, with SDL's clock stepping 221 ns. Over the 2000 ms tone SDL pulled exactly 96000 frames,
  where it pulled 95744 while the clock ran slow.
- **Rumble:** `XamInputSetState` ran controller 1's motors at half strength for 500 ms and stopped them.
- **Pacing's** longest wait for a buffer reads 17995 us where it read 17949 us: the same wait, at the true rate.

## The console's calling convention, register by register (2026-10-12)

Milestone 22's first step: a title of 195 checks drove the Xbox 360 SDK compiler's own code from both ends, so what
it measured is MSVC's convention, not GCC's (logs: `results/2026-10-12-convention-model/abi_model.log`, then with the
stack below r1 measured, `results/2026-10-12-convention-model-2/abi_model.log`, whose file was cut short in copying
and whose end comes from the title's debug output). MSVC's caller of each case called a stub that recorded r3-r10,
f1-f13 and 16 stack slots. That image went to MSVC's callee of the same prototype twice: whole, and with only the
registers and slots below named, garbage in all the rest. Each time the callee read what the caller meant, and
returns went the other way. All 195 checks passed in both runs, as in Xenia.
- **Arguments:** one 8-byte slot each, r3 to r10 and then the stack from 0x50(r1), each in the next register, with no
  padding for alignment, even before 8- and 16-byte-aligned structs. The cases here all fitted in r3 to r10; past
  them, a struct of more than 8 bytes starts on an even slot of the stack (corrected in "Calls between GCC and MSVC,
  with GCC calling as MSVC does").
  - A 32-bit value goes in its slot's low word, and the high half carries nothing: MSVC's caller zero-extended an
    `int` of -5 to `00000000FFFFFFFB`, and its callee widened arguments with garbage high halves right itself.
  - 64-bit integers go whole.
  - `float` and `double` go in f1 to f13 in turn, each also taking a slot, whose register or stack word the callee
    doesn't read. From the 14th on they go in their slots, a `float` as single precision in the slot's low word.
  - Structs go as their bytes: 1, 2 and 4 bytes right-justified like integers, every other size from its first
    slot's high byte across as many slots as it fills. Structs of floats and of doubles go the same way, never in
    FPRs.
  - Varargs each take their own slot, a `double` as its bits, past the 8th too. MSVC's caller also copies doubles
    into f1, f2 and on, but its variadic callee reads only the slots.
- **Returns:** `float` and `double` in f1, a `float` as a double. Structs of 1, 2, 4 and 8 bytes in r3, 1, 2 and 4
  right-justified, a struct of one `double` too. Every other size through a pointer the caller passes in r3, which
  comes back in r3. MSVC's caller widens a returned `int` itself; its callee returned -5 sign-extended.
- **Registers kept:** f14-f31: MSVC's caller held doubles there across a call, and lost 16 of 18 when the callee
  clobbered them. It held 18 vectors across a call in VMX128's v110-v127 rather than v14-v31, saved with
  `__savevmx_110`; GCC can't reach v64-v127. No CR field: conditions MSVC held across a call survived every field
  clobbered.
- **Frames:** MSVC's code writes LR at -8(r1) through r12 and its GPR saves under that, before `stwu`, then points r12
  under the GPR saves for `__savefpr_N` and `__savevmx_N`, which store below r12 (`addi r12,r1,-72` after saving
  r24-r31; xenia:src/xenia/cpu/xex_module.cc:1341-1455). Its callers keep nothing at 4-15(r1) across a call: a
  callee that overwrote those words broke nothing. Its callees don't write there.
- **Below r1:** the console writes there, but never within 1348 bytes of r1, with r1 16-byte aligned.
  - Callees that write nothing (an empty one, MSVC's leaf for an 8-byte struct, and MSVC's 15-double callee besides
    its own saves) saw a block from 1348 to 1792 bytes below r1 written in 1 run of 8.
  - The first run's every case showed it at 1792 bytes, each straight after a line was logged.
  - A 20 ms spin saw blocks from 1348 down to 2272 bytes in every run.
  - The blocks hold what looks like a thread's saved state: `801B0000`, the processor control region; addresses in
    the title; `24FFFF22`, like a CR value.
  - So code may use up to 1348 bytes below r1 without a frame, as MSVC's saves before `stwu` do. Xenia writes nothing
    there.

## Calls between GCC and MSVC, with GCC calling as MSVC does (2026-10-12)

Milestone 22's second step: xsdk on the `mcall-xsdk` branch, built with `-mcall-xsdk` from the toolchain patches there
into `~/xsdk-gcc-next` (logs: `results/2026-10-12-convention-compiler/`).

**Two counts.** MSVC counts the GPRs apart from the parameter area's slots. A struct of more than 8 bytes starts on an
even slot, a 16-byte boundary, whatever its own alignment; its GPRs, and those of what follows, come from the GPR
count, which that padding doesn't move. The cases of step 1 never showed it, since they fitted in r3 to r10. MSVC's code
for the model's new cases showed it (`local/abi/msvc_model.dis`), and the console confirmed it with `abi_model.log`,
whose callers sent and whose callees read each word where the two counts put it, with garbage in the rest:

| Before the struct | Struct | Its GPRs | Its slots | The int after it |
|---|---|---|---|---|
| 5 ints | 24 bytes | r8-r10, all of it | 6-8 | slot 9, 0x5C |
| 6 ints | 24 bytes | r9-r10, bytes 0-15 | 6-8 | slot 9 |
| 7 ints | 9, 12 (`u32` aligned), 16 (bytes or two doubles) | r10, bytes 0-7 | 8-9 | slot 10 |
| 7 ints | 24 bytes | r10, bytes 0-7 | 8-10 | slot 11 |
| 3 ints | 48 bytes | r6-r10, bytes 0-39 | 4-9 | slot 10 |
| 9 ints | 24 bytes | none | 10-12 | slot 13 |

A slot that holds part of a struct in a register carries nothing from the caller; the callee spills the register there.
A variadic callee spills the GPRs left into the slots after its named arguments: after an int and a 24-byte struct, r7
to r10 into slots 5 to 8, which reaches 0x50(r1), past the 64 bytes a caller always leaves. Its caller passed five
varargs, so that slot was its own; a caller that passes fewer gets 0x50(r1) written all the same. GCC now does the same
in both places, so the hazard is shared.

**The probe of 2026-10-11** (`abi_probe.log`): 36 of 36 cases passed, against 9 then. Layout agreed with `ms_struct` on
GCC's side: the struct mixing `u8` and `u32` bitfields is 12 bytes in both, with the same bytes, and `char16_t` is the 2
bytes of MSVC's `wchar_t`. MSVC's code changed CR6 across a call, which GCC no longer keeps anything in.

**Both directions** (`abi_model.log`): GCC builds the model's MSVC half too, and each case's caller and callee were
called across the two compilers. All 176 calls, structs of 1 to 24 bytes as arguments and returns, 15 doubles and 15
floats, varargs past 8 slots, widening, and every case above, delivered what MSVC's own pair delivers. Built with a GCC
that has a single count, the 10 cases whose struct is padded failed both ways, in Xenia; 2 of those 20 calls had
passed anyway until each call first scrubbed the stack below it, on values the previous call had left in the slot.

**xsdk_test** (`xsdk_test.log`): 218 of 219 checks passed.
- The kernel's way of calling a title, straight into GCC code holding 64-bit values, doubles and vectors, gave back
  r14-r31 whole, f14-f31 and v14-v31. Threads and the audio callback now take GCC code directly.
- `setjmp` and `longjmp` gave back all 54 registers after frames below them had overwritten every one.
- `KeQueryPerformanceFrequency`, declared with its 64-bit return, read 50000000.
- A C++ exception thrown through those frames gave back none of the 54: the catch's caller found every register as the
  deepest frame had left it. That run is the only one that did. With the unwinder's view of each frame logged
  (`repeats.log`), the catching frame held the original values and the catch got them. Then 400 throws a launch, half
  through the same empty catch as the failing run, kept every register in three launches: after a power cycle, straight
  after, and after abi_probe and abi_model as in the failing session, where each title also passed in full. The cause
  is unknown; the 400 throws stay in xsdk_test to catch it if it comes back.

**What xsdk does now.** A title calls an import through the loader's thunk alone, each its own symbol, and declares it
with its natural types: `imports/imports.txt` lists library, ordinal and name, and `gen_imports.py` checks no
signatures. The kernel calls a title's threads, audio callback and other callbacks directly, with no trampoline, and
the toolchain no longer keeps GCC off v14-v19.

**Mixing objects.** The assembler flags each object made under `-mcall-xsdk` (`EF_PPC_XSDK`,
tools/toolchain/binutils-xsdk.patch), and the linker refuses to put one in with an object without it, either way round
(binutils:bfd/elf32-ppc.c, `ppc_elf_merge_private_bfd_data`). The convention's own attribute, `.gnu_attribute 12`,
marks only objects that return small structs. The flag turned away the probe's harness and MSVC objects assembled by
the old toolchain, and every object in the new toolchain's libc, libstdc++ and libgcc carries it.

## What the kernel leaves a title (2026-10-12)

`build/xsdk_test.xex` at `a6b5743` and Dusklight's first-step `dusklight.xex` (Dusklight `e7e916c`, built with
`int32_t` predefined as `int` until the compiler makes it so; logs: `results/2026-10-12-memory/`, the xsdk_test one
the memory test's lines and the summary): all 222 checks passed, and both titles exited with status 0.
- **`MmQueryStatistics`** answers with the 104-byte result Xenia lays out
  (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:451-473) and refuses one 4 bytes short. Pages are 4 KB: 131072
  of them, 512 MB, the highest 0x1FFFF.
- **The pages add up:** the kernel holds 784 (3 MB) and the system 7408 (29 MB, 1503 of them free), which leaves the
  title 122880 pages, 480 MB, used and free together. xsdk_test, with 1964 in use, had 120916 free (472 MB); Dusklight
  442 MB.
- **The image** counts whole, in elf2xex's 64 KB pages: xsdk_test's 656 pages, and Dusklight's 8272, its 517 64 KB pages
  (33882112 bytes).
- **Heap growth takes free pages:** 16 MB through `sbrk`, every page touched, took 4100 pages, 16 MB and 4 more.
- **Address space:** the title has 767 MB, and `sbrk`'s 256 MB reservation shows as 256 MB reserved.
- **The rest of the title's pages,** the same in both: 256 stack (the main thread's 1 MB), 96 heap, 920 physical. The
  physical pages are taken before the title's first `malloc`, so not by the splash (see the next entry); what holds
  them isn't known. Dusklight's static constructors took 256 KB of newlib's heap.
- **`pread`** read at an offset without moving the position and read nothing at the end, and **`flockfile`** kept a
  second thread waiting while `stdout` was locked twice, then once.

## The heap reserves the title's free memory (2026-10-12)

`build/xsdk_test.xex` at `70e79b6` and Dusklight's first-step `dusklight.xex` relinked with it (logs:
`results/2026-10-12-heap-reservation/`, the xsdk_test one the reservation, the memory lines and the summary): all 223
checks passed, and both titles exited with status 0.
- **At the heap's first growth,** before `main`, `sbrk` reserved what `MmQueryStatistics` gave as the title's free
  memory: 472 MB in xsdk_test and 442 MB in Dusklight, after its 32 MB image. The kernel then shows 472 MB reserved.
- **The reservation covers what's free later:** the memory test found 120916 pages free, the 472 MB of the first
  growth, after the splash had run, and Dusklight 442 MB at `main`, as at its first growth. So the 920 physical pages
  both titles hold were taken before the first `malloc`, not by the splash.

## Dusklight's first step on the console's calling convention (2026-10-12)

`build/xsdk_test.xex` at `4302e7c`, this branch with main's `-mcall-xsdk` toolchain merged, and Dusklight's first-step
`dusklight.xex` (Dusklight `e7e916c`, SDL-xenon `c86389c89`), every object rebuilt from scratch with that toolchain and
no source changed (logs: `results/2026-10-12-dusklight-mcall-xsdk/`, the xsdk_test one its summary): all 228 checks
passed, and both titles exited with status 0.
- **Dusklight** started with the whole game linked, its heap reserving 443 MB at its first growth, and had 442 MB free
  at `main`, as on the old toolchain.
- **Its image** is 8192 pages, exactly 32 MB: 80 pages smaller than the old toolchain's, and again the image elf2xex
  packed, the 33570816-byte `.xex` less its 16 KB of headers.

## The switch to the console's calling convention (2026-10-12)

Milestone 22's third step: `~/xsdk-gcc` became the `-mcall-xsdk` toolchain, built from xsdk `8a349aa`, with the SysV
one kept at `~/xsdk-gcc-sysv`. Everything was rebuilt from clean on it and run on the console:
- **sdl_check** (`results/2026-10-12-switch/sdl_check-tail.log`), from the SDL fork at `c86389c89`, where the kernel
  now calls the audio callback directly: all 16 checks passed.
- **vulkan4xenon's test title** and **vkbench** (vulkan4xenon:results/2026-10-12-mcall-xsdk/), with titles no longer
  linking `crt0.o`: all 58 and all 14 checks passed. Against the run before the clock fix
  (vulkan4xenon:results/2026-10-11-milestone-8-reverted/), its times taken as 0.25% short:
  - `stream_8000`'s simulation took 1592 us through xsdk, from 1853, and 1596 through vulkan4xenon, from 1856.
  - xsdk's own recording ran 2 to 3% faster in every draw and state scene.
  - vulkan4xenon's recording ran 10.5 to 12% slower in the draw and state scenes, and its submit about 1% faster, so
    draws and submits together took 0.5 to 1.7% longer. Two runs of the same code before differed by up to 5% in
    recording. The layer's recording functions are the same instructions under both compilers; each non-leaf frame
    now reserves the 64-byte parameter area, so `vkCmdDraw`'s grew from 32 to 96 bytes. The cause is open (TODO.md).
  - GPU times moved within the 8% two runs of the same code differed by.
  - Frames read 16667 us, the true 60 Hz; vkbench prints that as 59.9 fps, cutting off to tenths.
- **Dusklight** ("Dusklight's first step on the console's calling convention", above): its first step passed
  again, its sources unchanged; `int32_t` as `int` cleared the type errors zlib-ng, imgui's FreeType glue, zstd's
  legacy decoders, Aurora and Dusklight had.

With that, milestone 22 is done: xsdk's GCC calls as the console's own compiler does, by default, and every title
built with it passes on the console.

## Dusklight runs up to the disc (2026-10-12)

`build/xsdk_test.xex` at `f770193` and Dusklight's second-step `dusklight.xex` (Dusklight `3adfb33`, Aurora `933efa4`,
SDL-xenon `c86389c89`) linked with it, run with no disc image beside it (logs: `results/2026-10-12-dusklight-step-2/`,
the xsdk_test one its summary): all 230 checks passed and xsdk_test exited with status 0. Dusklight ended with status
1, as it does without a disc.
- **Dusklight's own startup ran** on Aurora's null backend. It read its config (none yet, so the defaults), started
  Aurora with no window, and loaded RmlUi's eight font faces from `res\`. It then found no `game:\dusklight.iso`
  (0xC0000034, `STATUS_OBJECT_NAME_NOT_FOUND`; xenia:src/xenia/xbox.h:60) and ended there, since the game reads the
  disc's ID before `OSInit`.
- **`main` gets argc 1 and an empty `argv[0]`** (`6ea8d10`), which xsdk_test checks; Dusklight reads its options from
  `argv[1]` on.
- **Paths that `std::filesystem` makes absolute reach the file** (`f770193`). libstdc++ takes `game:\TwilitRealm\Dusk`
  for a relative path and puts the current directory, `game:\`, in front of it. xsdk turned that into
  `game:\game:\TwilitRealm\Dusk`, which the kernel refused (0xC0000033, `STATUS_OBJECT_NAME_INVALID`;
  xenia:src/xenia/xbox.h:59). The run before the fix (`dusklight-before.log`: the same Dusklight, its `Build:` line
  naming `4559590`, the commit `3adfb33` amended, linked with xsdk `6ea8d10`) logged 16 such `stat` failures, from
  borealis comparing its data folders through `weakly_canonical` (dusklight:extern/borealis/src/data.cpp:34-56). xsdk
  now starts a path over at a drive anywhere in it: this run logged none, and xsdk_test reads a file back through
  `absolute()` and `weakly_canonical()`.
- **Left as they are:**
  - The `mkdir` line is a folder that was already there (0xC0000035, `STATUS_OBJECT_NAME_COLLISION`;
    xenia:src/xenia/xbox.h:61).
  - The pipeline cache opens no database, since sqlite here has no VFS, and says "out of memory".
  - The window keeps its title, with no video subsystem.
  - The log carries Dusklight's colour codes.

## Dusklight runs the title scene (2026-10-12)

vulkan4xenon's milestone 9, step 3: Dusklight at `33e94c7` (Aurora `33c1b47`, SDL-xenon `c86389c89`), linked with xsdk
at `c98ceb8`, with Twilight Princess's GameCube disc (GZ2E01) beside it as `game:\dusklight.iso` (logs:
`results/2026-10-12-dusklight-step-3/`, each the part of the log that was sent). xsdk hasn't changed since its 230 of
230 above.
- **The title scene ran its minute** on Aurora's null backend, and the title exited with status 0 (`dusklight.log`).
  The game logs that minute only once stage F_SP102 has started. The run before the last fix
  (`dusklight-shutdown.log`), the same game up to its shutdown, gives the frame rate: 19.1 fps over the first 10
  seconds, as the scene loaded, then 29.6 to 30.0, as in Xenia.
- **At their peaks,** the game's root heap held 140176 KB of its 262127 and newlib's heap 385684 KB, 377 MB of the
  443 MB it reserves. The game's own heaps peaked as in Xenia, to the KB.
- **A float load from the command stream raised an alignment exception** (`dusklight-float-load.log`): 0x80000002 at
  an `lfsx` in Aurora's `handle_aurora`, loading a viewport's floats, which Aurora's stream packs at any byte. The
  integer loads from the same stream before it ran. GCC had merged an integer load and a `std::bit_cast` to float into
  that one float load, as it does where nothing swaps between them; Aurora `33c1b47` keeps it an integer load. Xenia
  doesn't check alignment, so it ran there.
- **A mutex relocked after its memory is freed corrupts the heap** (`dusklight-shutdown.log`): an xsdk mutex is a
  pointer to a critical section allocated on first use, and once its memory is freed that word is newlib's free-list
  link, which a lock then takes for the critical section. Dusklight's `OSResetSystem` woke the threads waiting on its
  message queues and freed the queues, mutexes included, before those threads relocked them. The title then crashed
  in `_free_r` (newlib's `_mallocr.c:2723`) with a write to 0xC, an unlink through a null link. With the queues kept
  (Dusklight `33e94c7`) it exited cleanly. Xenia never showed it.

## Floats at any address, milestone 23 (2026-10-12)

`~/xsdk-gcc` became milestone 23's toolchain, built from tools/toolchain/gcc-xsdk.patch at xsdk `e2a48fb`, with
milestone 22's kept at `~/xsdk-gcc-m22`. On xsdk's target GCC now moves a `float` or `double` between a float register
and memory only where it knows the memory is aligned to the value's size, as the console requires ("Dusklight runs the
title scene", above); any other goes through the integer registers, and an unaligned read reaches its float register
through the stack (`lwz`, `stw`, `lfs`). `-mcheck-fpr-alignment` makes any other float register access a compile
error. Logs: `results/2026-10-12-milestone-23/` and vulkan4xenon:results/2026-10-12-milestone-23/.
- **The check found none** in xsdk, SDL-xenon (`c86389c89`), vulkan4xenon (`a863279`), Aurora (`82dfa81`) or
  Dusklight (`a2aa9cc`), every object of each compiled with it.
- **xsdk_test passed 231 of 231** (`xsdk_test.log`), among them its new check: floats and doubles written at every
  offset from 0 to 7 and read back through `memcpy`, `std::bit_cast` and packed structs, 76 reads, none wrong.
- **Built with milestone 22's compiler, the same test stopped there** (`xsdk_test_unfixed.log`): the log ends at the
  check's first line, and nothing after it ran. That build has 14 float register loads and stores on the test's
  buffer, the fixed build none; offset 0 is aligned, so the first to fault is offset 1's `lfs f1,0(r3)` in
  `read_float_memcpy`. The exception itself wasn't captured.
- **sdl_check** passed 16 of 16 (`sdl_check.log`), **vulkan4xenon_test** 64 of 64 and **vkbench** 14 of 14
  (vulkan4xenon:results/2026-10-12-milestone-23/). Against milestone 22's vkbench run
  (vulkan4xenon:results/2026-10-12-mcall-xsdk/), on the same clock: the layer's draws and submits together took 0.4 to
  0.9% less in its draw and state scenes, xsdk's own within 1.2% either way, and GPU times within 2.6%. The larger
  percentages in the summary are of values of 3 to 5 us, where 1 us of rounding reads as 20%.
- **Dusklight ran as at vulkan4xenon's milestone 10** (`dusklight.log`, against
  vulkan4xenon:results/2026-10-12-milestone-10-step-2/dusklight.log): the boot screens at 50 fps, the title scene at
  29.7 to 30.0 fps once loaded, and exit status 0, with the same unsupported-feature errors and 9 MB free at the end,
  from 7. Aurora `33c1b47`'s integer read of the command stream's floats is still in, so the run shows that nothing
  else regressed; xsdk_test's `read_float_bits` is that read.

With that, milestone 23 is done: xsdk's GCC keeps float registers off memory it doesn't know is aligned, and every
title built with it passes on the console.

## Pixel shaders past 16 registers, milestone 24 (2026-10-12)

xsdk's set-up now stops stepping the shader unit's register split at the kernel's own 0x00040400, 64 registers for
pixel shaders and 64 for vertex shaders (xsdk `2562309`), where libxenon's steps go on to 0x00070100, 16 and 112
("libxenon's set-up makes the resolves work", above). The fields are Adreno 2xx's: pixel shaders' share in bits 4-11
of `SQ_GPR_MANAGEMENT`, vertex shaders' in bits 12-19 (Linux:drivers/gpu/drm/msm/registers/adreno/a2xx.xml:1231-1235).
xost builds for that split and refuses a shader past its share (xost `67994ad`), as do vulkan4xenon's builds
(vulkan4xenon `3368b84`) and Dusklight's shader pack (Aurora `28a3bc1`, Dusklight `6e6c9c6`). Logs:
`results/2026-10-12-milestone-24/` and vulkan4xenon:results/2026-10-12-milestone-24/.
- **xsdk_test passed 241 of 241** (`xsdk_test.log`). Its new checks draw with pixel shaders using 18 and 64 registers
  and a vertex shader using 64, each filling every register with a constant of its own and adding the square of how far
  each is off into its colour (tools/gen_register_shaders.py): all three quads held their colours in every one of the
  59 frames checked.
- **Under libxenon's split the 18-register draw hung the GPU** (`xsdk_test_libxenon_split.log`): its fence never
  landed in the first frame, and the GPU hadn't finished at close (0xC00000B5, so xsdk kept it and its memory); the
  231 checks before it passed. That is xost's "r16 hangs, r15 runs"
  (xost:xasm/program.py), from the split.
- **Broken on purpose,** the 18-register shader writing r17's fill into r16, as two registers sharing would, washed
  that quad to white in every frame, FFFFFFFF where 208020FF was due, and passed the other two
  (`xsdk_test_alias.log`).
- **vulkan4xenon_test** passed 64 of 64 and **vkbench** 14 of 14 (vulkan4xenon:results/2026-10-12-milestone-24/).
  Against milestone 23's run the CPU's draw and submit times moved within 1.4%, and most GPU times within 2% though
  vertex shaders went from 112 registers to 64; the state scenes' GPU times fell, xsdk's `states_250` from 793 to 591 us and
  `states_1000` from 842 to 665, and vulkan4xenon's `states_250` from 608 to 420, while vulkan4xenon's `states_1000`
  stayed at 305. Why the split helps those three and not the fourth isn't looked into.
- **Dusklight** (`dusklight.log`), with its pack's 69th program, the 18-register `e3fba406929fde97.frag`, ran the boot
  screens and the title scene as at milestone 23 and exited with status 0; the backend no longer reports that program
  missing from the pack, and its other errors are milestone 23's.

With that, milestone 24 is done: pixel shaders get 64 registers, and every title built for it passes on the console.

## Each title's log is named after its .xex (2026-10-12)

xsdk names a title's log after the `.xex` the loader started (xsdk `7b23184`), from the path the kernel leaves in
`ExLoadedImageName`, xboxkrnl's export 0x1AF (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_module.cc:177-185). On the
console it held the path the title was launched from: `\Device\Harddisk0\Partition1\XSDK\xsdk_test.xex` and
`\Device\Harddisk0\Partition1\dusklight\dusklight.xex`, so the logs came out as `game:\xsdk_test.log` and
`game:\dusklight.log` (`results/2026-10-12-milestone-24/`).
