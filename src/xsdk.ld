/* Lays a title out for elf2xex: read-only, code and writable data each start on their own 64 KB page. */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(xsdk_start)
/* It lives in libxsdk.a, which only the entry point refers to. */
EXTERN(xsdk_start)

SECTIONS
{
	/* Titles load at 0x82000000; elf2xex puts the PE headers in the first 4 KB. */
	. = 0x82000000 + 0x1000;
	.rdata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		*(.sdata2 .sdata2.* .sbss2 .sbss2.*)
		*(.gcc_except_table .gcc_except_table.*)
	}
	/* start.c registers this with libgcc's unwinder; the zero word ends the list, as crtend.o would. */
	.eh_frame : {
		__eh_frame_start = .;
		KEEP(*(.eh_frame))
		LONG(0)
	}

	. = ALIGN(0x10000);
	.text : {
		/* Imports first: Xenia runs a tail call's target as the caller (xenia:src/xenia/cpu/ppc/ppc_scanner.cc:204). */
		*(.text.xsdk_thunk_*)
		*(.text .text.*)
	}

	/* The constructor and destructor lists are writable sections, so they go with the data. */
	. = ALIGN(0x10000);
	.preinit_array : {
		__preinit_array_start = .;
		KEEP(*(.preinit_array))
		__preinit_array_end = .;
	}
	.init_array : {
		__init_array_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array))
		__init_array_end = .;
	}
	.fini_array : {
		__fini_array_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
		KEEP(*(.fini_array))
		__fini_array_end = .;
	}
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
