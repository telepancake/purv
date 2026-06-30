/* purv four-region guest layout: code (text + rodata) in the read-only lower half
 * at 0; writable data (data + bss) in the upper half at 0x80000000 (the host's
 * heap region, which also holds the malloc arena past the loaded data). The guest
 * is built -mcmodel=medany, so the text reaches the data pc-relatively. */
SECTIONS {
  . = 0x00000000;
  .text : {
    *(.text._start)
    *(.text .text.*)
    *(.rodata .rodata.* .srodata .srodata.*)
    *(.data.rel.ro .data.rel.ro.*)
  }

  . = 0x80000000;
  .data : { *(.data .data.* .sdata .sdata.*) }
  .bss (NOLOAD) : { *(.sbss .sbss.* .bss .bss.* COMMON) }
  _end = .;

  /DISCARD/ : {
    *(.comment) *(.note .note.*) *(.riscv.attributes)
    *(.eh_frame) *(.eh_frame_hdr)
  }
}
