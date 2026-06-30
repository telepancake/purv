/* purv guest layout for the four-region engine:
 *   - code (text + rodata) in the read-only lower half, based at 0;
 *   - writable data (data + bss) in the upper half, based at 0x80000000 (heap).
 * The entry point is set on the ld command line (-e _start, or --entry=0 for the
 * --invoke library examples); the host runs from the ELF e_entry. */
SECTIONS {
  . = 0x00000000;
  .text : {
    *(.text._start)
    *(.text .text.*)
    *(.rodata .rodata.* .srodata .srodata.*)
  }

  . = 0x80000000;
  .data : {
    *(.data .data.* .sdata .sdata.*)
    *(.data.string)
  }
  .bss (NOLOAD) : { *(.sbss .sbss.* .bss .bss.* COMMON) }
  _end = .;

  /DISCARD/ : {
    *(.comment) *(.note .note.*) *(.riscv.attributes)
    *(.eh_frame) *(.eh_frame_hdr)
  }
}
