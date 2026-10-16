# Milestone 40: catching faults

Agreed with the user on 2026-10-15, as the first of the gaps docs/PARITY.md ranks, as written: no function names on
the console and no title callback.

## Why

Nothing in xsdk catches a CPU exception: a bad pointer, a misaligned store or an illegal instruction.
- **What happens today is unrecorded.** A title made to store to address 0 didn't end on 2026-10-13. Its `main` ran
  again in the same image, its data as the fault left it, until the title stopped 12 s later. That run was a side
  check, so the probe below records it properly first.
- **What a fault leaves behind.** The log stops mid-run with no line about it. The faults found so far were seen only
  through xbWatson, on a console that runs Microsoft's debug monitor (docs/HARDWARE.md, "XEX checks the kernel
  enforces", "C++23 runtime, milestone 3", "A ring without VdInitializeEngines"). Once, the title carried on past an
  alignment fault.
- **Who reads the log.** Most people will port games through an AI assistant, which can't see the screen or attach a
  debugger. An exception code, an address and a stack in the log are what it needs to find the bug.

## What it takes

1. **A probe**, `tests/probe_fault.c`, settles what the console does, as nothing public says.
   - **No handler.** It stores to address 0 and records what follows: `main` again, the dashboard or a hang, and how
     long it takes. The console runs the debug monitor `Xbdm.xex`, which may be what carries a faulting title on, so
     the record says so: a console without it may differ.
   - **The hook.** It imports `RtlSetVectoredExceptionHandler` (0x36D) and `RtlClearVectoredExceptionHandler` (0x36E)
     (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc:891-892). Xenia has no code for them, so their arguments come
     from the console.
     - It calls the first one so that either likely form works: Windows' `AddVectoredExceptionHandler(first,
       handler)` (Microsoft's public Win32 documentation) or a lone handler. It does that by passing the handler as
       both arguments.
     - It records whether the handler runs, on which thread, what it is passed, and which return values resume and
       which pass the fault on.
   - **The exception record** follows Xenia's `X_EXCEPTION_RECORD` (xenia:src/xenia/xbox.h:314-323). The probe checks
     the code, the address, and the parameters an access violation carries.
   - **The register context** has no public layout for the console. To find it, the probe sets r14-r31, CTR and
     f14-f31 to known patterns, then faults at an instruction whose address it knows. It dumps the context and finds
     each pattern's offset, then IAR, LR, CR, XER and r1.
   - **The faults:**
     - a store to address 0, and a load from it;
     - a misaligned 64-bit store into write-combined memory (docs/HARDWARE.md, "A ring without VdInitializeEngines");
     - an illegal instruction;
     - a trap (`__builtin_trap`);
     - the same store on a thread placed on each hardware thread;
     - a stack overflow;
     - a fault while the GPU and the display are open and presenting.
   - **Ending.** It records whether `XamLoaderTerminateTitle` from the handler takes the console back to the dashboard
     while the GPU and display are still held, or whether xsdk must give them back first, as `_exit` does
     (`src/exit.c`).
2. **The handler**, `src/fault.c`, installed by `xsdk_start` before the constructors run, so that a fault in a
   constructor is caught too. On a fault it writes a report into the title's log, then ends the title back to the
   dashboard. The report gives:
   - the exception's code and name, the faulting instruction's address, and, for an access violation, the address
     accessed and whether it was a read or a write;
   - the thread: its xsdk thread and its hardware thread;
   - r0-r31, LR, CTR, CR and XER;
   - the stack, as return addresses along the back chain from the faulting r1. xsdk's GCC saves a frame's return
     address 4 bytes into its caller's frame, in the SysV slot (its `stwu r1,-128(r1)` then `stw r0,132(r1)`).
     MSVC's code, the kernel's and XAM's, saves it 8 bytes below (docs/HARDWARE.md, "The console's calling
     convention, register by register"). So the walk takes whichever slot holds an address in code, and it is
     checked against a known chain of calls.
   - Every read is checked against the thread's stack bounds, from its processor control region (docs/HARDWARE.md,
     "The kernel's calls into a title keep its registers"), so a broken chain ends the walk instead of faulting
     again. The walk stops at 32 frames.

   It's written for a title that's already broken. It allocates nothing. It takes the log's lock with a time limit,
   and writes without it if another thread still holds it. A fault inside the handler ends the title with one line,
   instead of looping.
3. **abort and std::terminate.** They log the same stack from where they were called. libstdc++ already writes an
   uncaught exception's type and `what()` to `stderr`, which goes to the log.
4. **Names.** `tools/symbolize.py` turns a report's addresses into function, file and line. It does that with the
   toolchain's `addr2line` and the title's ELF, which keeps its debug information at the XEX's addresses.
5. **A test title**, `tests/fault_check.c`. It runs each of the probe's faults in turn and checks its report. If the
   probe finds the handler can resume, all of them run in one launch; otherwise it's one fault a launch, picked by a
   file beside the `.xex`, as `net_host.txt` is. `tools/check_fault_log.py` checks each report:
   - the code;
   - the faulting address against the labelled instruction;
   - the address accessed;
   - the known register patterns;
   - the stack against the expected chain of functions;
   - the thread;
   - a last line saying the title ended.

## Done when

- docs/HARDWARE.md records the probe's results from the console.
- **On the console,** every fault the handler can catch gets a correct report, and the title ends back at the
  dashboard with no reboot. Faults it can't catch are recorded with what happens instead; a stack overflow may be one.
- **Broken on purpose,** each break fails its check:
  - the handler not installed: no report;
  - the stack walk reading the return address from the wrong offset: the chain check fails;
  - two register offsets swapped: the pattern check fails.
- **xsdk_test and vulkan4xenon_test** pass on the console, built with the handler, which every title now gets.

## Not in it

- **Hangs that aren't CPU exceptions:** a wedged GPU, a title stuck in a loop, the kernel or XAM crashing, or the whole
  console locking up. The handler sees none of them. The GPU's waits already time out (`xsdk_gpu_wait`), but
  `VdShutdownEngines` never returns while the GPU is wedged (`src/gpu.c`), so such a title still can't end.
- **Crash dumps and a GDB stub.** Both build on this.
- **The log over the network.**
- **A title's own callback** on a fault, such as to save the game first.
- **Resuming after a fault,** except as the probe's and the test's tool.
- **Function names on the console,** from a symbol map beside the `.xex` (TODO.md); the PC tool names them for now.
