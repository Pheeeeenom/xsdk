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
