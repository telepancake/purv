OUTPUT_ARCH( "riscv" )
ENTRY(rvtest_entry_point)

/* purv four-region layout: code (text) in the read-only lower half at 0; the
 * writable data, signature, and tohost word in the upper half at 0x80000000
 * (the host's heap region). Tests are built -mcmodel=medany, so the text reaches
 * the data pc-relatively. */
SECTIONS
{
  . = 0x00000000;
  .text.init : { *(.text.init) }
  . = ALIGN(0x1000);
  .text : { *(.text) }

  . = 0x80000000;
  .tohost : { *(.tohost) }
  . = ALIGN(0x1000);
  .data : { *(.data) }
  .data.string : { *(.data.string)}
  .bss : { *(.bss) }
  _end = .;
}
