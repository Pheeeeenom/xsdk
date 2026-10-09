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
  The test log lands in its `cache\xsdk.log`, and `DbgPrint` output in its `xenia.log`.
- Xenia skips instructions it doesn't implement, silently (xenia:src/xenia/cpu/ppc/ppc_emit_*.cc,
  `XEINSTRNOTIMPLEMENTED`). GCC emits one of them, `mcrf`; newlib's printf core uses it, so in Xenia every formatted
  conversion gains two NUL bytes. Each link runs `tools/xenia_scan.py`, which lists such instructions by function;
  treat any Xenia result that goes through those functions as unreliable.

## Build

- The toolchain runs in WSL Debian. Until the planned GCC 13+ toolchain lands, it's the xenon GCC 9.2 at
  `/usr/local/xenon`. Call it from PowerShell with `wsl.exe -d Debian --exec bash -c '...'`; Git Bash rewrites its
  absolute paths.
- clang-format is at `C:\Program Files\LLVM\bin\clang-format.exe`.
- Build the SDK and the test title (`build/xsdk_test.xex`) from PowerShell:

```
wsl.exe -d Debian --exec bash -c 'cd /mnt/z/Xbox-360-SDK && make'
```
