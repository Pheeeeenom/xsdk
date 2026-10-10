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

An xbWatson minidump's system information reports build 21256, but Aurora's D3D hang report on the same console says
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
- **Aurora leaves the command processor running and idle.** 0x07D8 is 0x00007FFF, so bit 28 (halted) is clear, and the
  busy bit of 0x1740 is clear. The ring registers still describe Aurora's ring: base 0x01B9D000, control 0x00020309,
  read pointer write-back at 0x01B9F03C, read and write pointers both 0xEB.
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
needs. The probe now reads only the ME RAM, which is safe.

## A ring without VdInitializeEngines (2026-10-10)

`tests/probe_ring.c` skipped `VdInitializeEngines` and used the command processor as Aurora left it, running with its
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
- **Aurora's D3D printed a GPU hang report** when it started again. It names 0x1740 `RBBM_STATUS` (0x80050110) and the
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
