# Code guidelines

These rules apply to all code in this repository, and to the ports' forks as section 14 says. Their purpose is to keep
the SDK small, predictable and correct on real hardware.

If a rule gets in the way of correct code, change the rule in this file first, in its own commit. Don't quietly break it.

## 1. Principles

1. **Keep it simple.** Write the most direct code that is correct. Don't add an abstraction, helper layer or generic
   mechanism until at least three real call sites need it.
2. **Build only what is used.** Implement a kernel import, runtime piece or GPU feature when a milestone in SCOPE.md or
   a test needs it, not before. No speculative options, flags or extension points.
3. **Cite the source.** Every fact about the kernel, the XEX format or Xenos comes from Xenia, libxenon, xost, other
   public reverse engineering, or a hardware result, and the code or commit says which. Anything else is marked
   unverified until hardware settles it.
4. **Measure, don't guess.** Performance claims need numbers from the console. An optimization that can't show a
   measured win is reverted.
5. **Be honest about support.** Never fake success. Anything not implemented fails with an error
   (`STATUS_NOT_IMPLEMENTED`, `ENOSYS`).

## 2. Language and build

- Console code is C17 with GNU extensions (`-std=gnu17`). C++23 appears only in the C++ runtime tests and samples.
- Assembly only where C can't do it (entry point, import stubs, callback trampolines), in `.S` files run through the
  preprocessor. Assembly never writes `r2` or `r13`: `r13` holds the processor control region.
- Host tools are Python 3.11 or later, standard library only, and run both on Windows and in WSL.
- Zero warnings: our C builds with `-Wall -Wextra -Werror`.
- Dependencies are the xenon toolchain (GCC, binutils, newlib) only. No libxenon in titles: it drives bare metal, not
  the retail kernel. Adding a library needs a reason in the commit message.
- No Microsoft tooling, no leaked SDK material, no OpenXeChain.
- Never commit Microsoft binaries, retail executables, firmware or kernel dumps, keys, the command processor microcode
  (SCOPE.md section 3.2), XDK compiler output, or build output. `local/` holds files that must stay out of git, and so
  do the reference clones in `xenos-toolchain/reference/`.
- Code or tables taken from Xenia or libxenon keep their BSD-3 notice and name their source file.

## 3. Formatting

- `.clang-format` is the authority on C and C++ layout. Run it on every changed file before committing:
  `clang-format -i <files>`.
- Tabs for indentation, 120-column limit, function braces on their own line, all other braces on the same line.
  Wrapped lines are indented one tab, never aligned, so they look right at any tab width.
- If clang-format produces an awkward wrap, restructure the code (shorter names, a local variable, a helper). Don't fight
  the formatter.
- Never disable the formatter with `// clang-format off`.
- Assembly: one instruction per line, tab-indented, `//` comments.
- Python: PEP 8, 4-space indentation, 120-column limit.

## 4. Naming

| Kind | Style | Example |
|---|---|---|
| Functions, variables | `snake_case` | `xsdk_log_write` |
| Types (`typedef struct`, `typedef enum`) | `PascalCase` | `ImportRecord` |
| Macros, enum values, constants | `UPPER_SNAKE_CASE` | `XEX_PAGE_SIZE` |
| Kernel and XAM exports | exact export name in Xenia's tables | `NtCreateFile`, `XamInputGetState` |
| Kernel ABI types and constants | NT name as in Xenia, without its `X_` prefix | `OBJECT_ATTRIBUTES`, `STATUS_SUCCESS` |
| GPU registers and PM4 opcodes | name in Xenia's `register_table.inc` and `xenos.h` | `RB_SURFACE_INFO`, `PM4_EVENT_WRITE` |

- Titles link our code into their own, so every non-`static` symbol starts with `xsdk_`, then the module
  (`xsdk_log_write` in `log.c`). File-local functions are `static` and need no prefix.
- Put units in names when they aren't obvious: `size_bytes`, `timeout_ms`, `pitch_pixels`.
- No abbreviations beyond well-known ones (`gpu`, `cp`, `pcr`, `tls`, `xex`, `pe`, `fb`, `idx`).

## 5. Files and modules

- One concern per `.c` file. Its `.h` declares only what other files use; everything else is `static`.
- Headers are self-contained, have include guards named after the component and file (`XSDK_LOG_H`), and include only
  what they use.
- Include order: the file's own header, then `<system and toolchain headers>`, then `"project headers"`, each group
  sorted.
- No mutable state shared between files. Module state lives in `static` variables in that module.

## 6. Functions

- A function does one thing. If describing it needs "and", split it.
- Keep functions readable on one screen. Long straight-line setup is fine; nesting deeper than three levels is not.
  Restructure, usually with early returns.
- Return early on errors so the main path stays unindented.
- Mark everything `const` that isn't modified, including pointer parameters.
- Pass sizes and counts explicitly, next to the buffer they describe.

## 7. Macros

- Use macros only for constants, include guards and trivial expressions that can't be a `static inline` function.
- No control flow inside macros: no hidden `return`, `goto` or `break`. Write error checks out.
- Prefer `enum` and `static const` over `#define` for typed constants.

## 8. Errors

- Check every call that can fail at runtime: kernel calls, allocation, file IO. Log the call and the status as
  `0x%08X`.
- SDK functions that wrap the kernel return its `NTSTATUS`. The newlib and POSIX layers follow POSIX: they set the
  closest `errno` and return what POSIX says.
- On failure, release what was acquired so far in reverse order. A single `goto fail` cleanup label per function is
  allowed when it's simpler than nested ifs; no other use of `goto`.
- Tests and samples that exit on failure may leave cleanup to title exit. The SDK may not.
- Don't discard return values silently. If a result truly doesn't matter, cast it to `(void)`.
- Caller bugs that break a documented precondition are caught with `assert` in debug builds, not checked at runtime on
  the hot path.

```c
static NTSTATUS open_log(HANDLE *handle, const char *path)
{
	ANSI_STRING name;
	RtlInitAnsiString(&name, path);
	OBJECT_ATTRIBUTES attributes = { .name = &name };
	IO_STATUS_BLOCK io;

	NTSTATUS status = NtCreateFile(handle, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io, NULL,
		FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT);
	if (!NT_SUCCESS(status)) {
		xsdk_log_error("NtCreateFile(%s) failed: 0x%08X", path, status);
		return status;
	}
	return STATUS_SUCCESS;
}
```

## 9. Memory and the hot path

- The **hot path** is GPU command building and submission: writing PM4 packets, kicking the ring and presenting. It
  has no locks, no logging except when returning an error, and no heap allocation. Allocate at creation; per-frame
  data comes from preallocated rings or pools.
- Physical memory goes through one module. Every allocation has a matching free.
- One exception: while the GPU is wedged, `xsdk_gpu_close` keeps the ring and its write-back page, since the GPU may
  still use them.
- Every object that owns memory has a destroy function that releases it in reverse creation order.
- Fixed limits are named constants. If a number's origin isn't obvious, give it in one line.

## 10. Hardware behavior

- The retail kernel on the console decides what is correct. Where Xenia and the console differ, the console wins, and
  the code says so in one line.
- Something that touches the loader, the kernel ABI or the GPU isn't proven until it runs on the console.
- `docs/HARDWARE.md` records each fact the console has settled: what was tested, the result, and the date. Update it in
  the same commit as the code that depends on it.

## 11. Comments

- Comments are rare, one line, and simple enough for a five-year-old. No paragraphs and no comment blocks.
- If one simple line can't explain it, don't comment. Make the code clearer, and put longer reasoning in the commit
  message.
- Comment *why*, never *what*.
- Behavior no header in this repository states gets one line naming its source, in SCOPE.md's form:
  `// r13 holds the thread's PCR (xenia:src/xenia/cpu/thread_state.cc:55)`.
- No commented-out code. A TODO is one line saying what's missing.

## 12. Testing

- The console is the source of truth. A feature is done when it builds without warnings, runs on the RGH console, and
  its test passes. Xenia Canary counts only as a crash and smoke check: it skips the XEX checks and emulates the kernel
  at a high level.
- Each feature has a small test under `tests/` that exercises it and writes a pass/fail line to the test log. Tests
  share one title, so a hardware check is one upload and one launch.
- Keep hardware results under `results/<date>-<description>/`.

## 13. Commits

- One logical change per commit. Formatting-only changes get their own commit.
- Message: an imperative summary under 72 characters, then a body explaining why. Include console numbers for
  performance changes.
- Every commit builds.

## 14. Ports

Dusklight and Aurora are forks of other projects (vulkan4xenon:docs/ROADMAP.md, section 5), kept close to upstream so
their changes merge.

- Their language, layout and naming are theirs: C++20 and the fork's `.clang-format`, not sections 2 to 5.
  Dusklight's own rule holds: game code changed for the port sits under a target `#if`, the original kept
  (dusklight:docs/code-conventions.md).
- Everything else here applies: the principles, errors, comments, citations, measuring, testing on the console and
  commits.
- Libraries a fork brings stay as upstream ships them; each one kept is listed with its reason.
