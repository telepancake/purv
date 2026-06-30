/* Flat-binary layout: no ELF/program headers at all. Everything goes at the load
 * address with _start first, so the raw image (objcopy -O binary) starts with the
 * entry point at offset 0. The host loads the bytes at 0 (the read-only code
 * region) and jumps there; writable data is the heap, reached via malloc. */
ENTRY(_start)

SECTIONS {
  . = 0x00000000;

  .text : {
    *(.text._start)                         /* entry must be first */
    *(.text .text.*)
    *(.rodata .rodata.* .srodata .srodata.*)
    *(.data .data.* .sdata .sdata.*)
  }

  .bss (NOLOAD) : {
    *(.sbss .sbss.* .bss .bss.* COMMON)
  }

  /DISCARD/ : {
    *(.comment) *(.note .note.*) *(.riscv.attributes)
    *(.eh_frame) *(.eh_frame_hdr)
  }
}
