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

The console's kernel is build 21256, from the system information in an xbWatson minidump (2026-10-10). The same
dump's module list names the kernel `xboxkrnld.exe` and XAM `xamd.dll`, with `Xbdm.xex` loaded. The patch set isn't
recorded; another patch set may differ.

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
