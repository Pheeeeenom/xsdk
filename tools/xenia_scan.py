#!/usr/bin/env python3
"""List instructions Xenia skips, from `objdump -d` output on stdin, so a Xenia run can be judged."""

import re
import sys

# Emitters that only call XEINSTRNOTIMPLEMENTED (xenia:src/xenia/cpu/ppc/ppc_emit_*.cc).
SKIPPED = {
    "mcrf",  # ppc_emit_control.cc:418
    "lswi", "lswx", "stswi", "stswx",  # ppc_emit_memory.cc:713-731
    "mcrfs", "mffs.", "mtfsb0", "mtfsb0.", "mtfsb1", "mtfsb1.",  # ppc_emit_fpu.cc:353-376
    "rldic", "rldic.",  # ppc_emit_alu.cc:916
    "mfvscr", "mtvscr", "vmhaddshs", "vmhraddshs", "vmladduhm", "vmsummbm", "vmsumshm", "vmsumshs", "vmsumubm",
    "vmsumuhm", "vmsumuhs", "vmulesb", "vmulesh", "vmuleub", "vmuleuh", "vmulosb", "vmulosh", "vmuloub", "vmulouh",
    "vsumsws", "vsum2sws", "vsum4sbs", "vsum4shs", "vsum4ubs", "vpkpx", "vupkhpx", "vupklpx",  # ppc_emit_altivec.cc
}
# Xenia doesn't set XER[OV], so the overflow-enable forms are wrong (xenia:src/xenia/cpu/ppc/ppc_emit_alu.cc:48-536).
OVERFLOW_FORMS = re.compile(r"(add|addc|adde|addme|addze|divd|divdu|divw|divwu|mulld|mullw|neg|subf|subfc|subfe"
    r"|subfme|subfze)o\.?")

FUNCTION = re.compile(r"^[0-9a-f]+ <(.+)>:$")
INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\t(?:[0-9a-f]{2} ){4}\t(\S+)\s*(.*)$")


def main():
    function = "?"
    found = 0
    for line in sys.stdin:
        line = line.rstrip("\n")
        match = FUNCTION.match(line)
        if match:
            function = match.group(1)
            continue
        match = INSTRUCTION.match(line)
        if not match:
            continue
        address, mnemonic, operands = match.groups()
        if mnemonic in SKIPPED or OVERFLOW_FORMS.fullmatch(mnemonic):
            print(f"xenia_scan: {function}: {address} {mnemonic} {operands}")
            found += 1
    if found:
        print(f"xenia_scan: {found} instructions Xenia skips; Xenia results through these functions are unreliable")


if __name__ == "__main__":
    main()
