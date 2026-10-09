/* Lays a title out for elf2xex: read-only, code and writable data each start on their own 64 KB page. */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(_start)

SECTIONS
{
	/* Titles load at 0x82000000; elf2xex puts the PE headers in the first 4 KB. */
	. = 0x82000000 + 0x1000;
	.rdata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		*(.sdata2 .sdata2.* .sbss2 .sbss2.*)
	}
	.eh_frame : {
		KEEP(*(.eh_frame))
	}

	. = ALIGN(0x10000);
	.text : {
		*(.text .text.*)
	}

	. = ALIGN(0x10000);
	.data : {
		*(.data .data.* .gnu.linkonce.d.*)
		*(.sdata .sdata.*)
	}
	.bss : {
		*(.sbss .sbss.* .bss .bss.* COMMON)
	}

	.xsdk.imports 0 : {
		KEEP(*(.xsdk.imports))
	}

	/DISCARD/ : {
		*(.comment .note.* .gnu.attributes)
	}
}
