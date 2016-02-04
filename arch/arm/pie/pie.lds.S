OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)

#include <linux/export.h>

SECTIONS
{
	/* Don't need unwind tables */
	/DISCARD/ : {
		*(.ARM.exidx*)
		*(.ARM.extab*)
		*(.comment)
	}

	. = 0x0;

	____pie_PIE_NAME_start : {
		VMLINUX_SYMBOL(__pie_PIE_NAME_start) = .;
	}

	.text : {
		. = ALIGN(4);
		KEEP(*(.text))
	}

	.rodata : { *(SORT_BY_ALIGNMENT(.rodata*)) }
	. = ALIGN(4);

	.data : {
		*(SORT_BY_ALIGNMENT(.data*))
		. = ALIGN(4);

		*(SORT_BY_ALIGNMENT(.bss*))
		. = ALIGN(4);
	}

	____pie_PIE_NAME_end : {
		VMLINUX_SYMBOL(__pie_PIE_NAME_end) = .;
	}
}
