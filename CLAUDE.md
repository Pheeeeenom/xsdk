# Xbox 360 SDK

A from-scratch homebrew SDK that builds `.xex` titles for the retail kernel and dashboard on an RGH console, with a GPU
layer that drives Xenos directly. Scope, sources and milestones are in SCOPE.md.

## Working rules

- Follow CODE_GUIDELINES.md for all code. If a request conflicts with it, say so before writing code.
- Agree on scope before starting a feature. Build only what was agreed, and don't build ahead. The agreed scope is
  SCOPE.md.
- Back up any claim about the kernel, the XEX format or Xenos with evidence: Xenia, libxenon or xost source (under
  `xenos-toolchain/reference/`, cited as in SCOPE.md), other public reverse engineering, or a hardware result. Say
  which. No Microsoft tooling, no leaked SDK material, no OpenXeChain.
- Hardware runs are on the user's RGH console. The user copies the `.xex` over FTP, launches it, and sends back the
  log. Ask for a hardware run only when a milestone or a question about the real kernel needs one.
- Xenia Canary (`C:\Users\Mena\Downloads\xenia_canary\xenia_canary.exe`, which may be moved) is for crash and smoke
  checks only. Launch it with the `.xex` path; it stays open after the title exits, so stop it after a few seconds.
  A title's log lands in its `cache` folder, named after the `.xex` (`cache\xsdk_test.log`), and `DbgPrint` output
  in its `xenia.log`. For the GPU tests add
  `--readback_resolve=full`, so the CPU sees what resolves write, and `--async_shader_compilation=false`, or Xenia
  skips draws while it compiles a new shader. Xenia has no display controller, so the display checks fail there. It
  runs the time base at the 50 MHz the kernel reports, not the console's 49.875 MHz, so the check that the SDK's
  clock keeps the kernel's time fails there too (docs/HARDWARE.md, "The time base runs at 49.875 MHz"). Its
  `MmQueryStatistics` guesses reserved bytes and counts free pages in its physical heaps only, so the heap
  reservation and heap growth checks fail there as well (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:500-525).
  It runs each of the title's threads on a host thread of its own (xenia:src/xenia/kernel/xthread.cc:378), so two
  busy threads placed on one hardware thread don't share it there, and that placement check fails too.
- Xenia skips instructions it doesn't implement, silently (xenia:src/xenia/cpu/ppc/ppc_emit_*.cc,
  `XEINSTRNOTIMPLEMENTED`). GCC emits some of them: `mcrf`, rare now that the toolchain reserves CR2-CR7, and `rldic`
  under `-mpowerpc64`. Each link runs `tools/xenia_scan.py`, which lists such instructions by function; treat any Xenia
  result that goes through those functions as unreliable.
- Xenia finds a function's end by scanning, and a forward `b` makes it take everything up to the target as one function
  (xenia:src/xenia/cpu/ppc/ppc_scanner.cc:204). Tail calls into that range then run the wrong code or crash Xenia. The
  toolchain turns off tail calls and hot/cold splitting, and `src/xsdk.ld` puts import stubs first; keep it that way.
- Write memory the GPU reads (write-combined or uncached) through `volatile` 32-bit stores. GCC merges plain stores
  into 64-bit `std`, and on the console one that isn't 8-byte aligned raises an alignment exception there
  (docs/HARDWARE.md, "A ring without VdInitializeEngines").

## Build

- The toolchain runs in WSL Debian: GCC 15, binutils and newlib for `powerpc-xsdk-elf`, installed at `~/xsdk-gcc`.
  `tools/toolchain/build.sh` builds it (`JOBS=32` uses every core); its specs and patches say what differs from a
  stock build. Call WSL from PowerShell with `wsl.exe -d Debian --exec bash -c '...'`; Git Bash rewrites its absolute
  paths.
- clang-format is at `C:\Program Files\LLVM\bin\clang-format.exe`.
- Build the SDK and the test title (`build/xsdk_test.xex`) from PowerShell:

```
wsl.exe -d Debian --exec bash -c 'cd /mnt/z/Xbox-360-SDK && make'
```
