/* purva guest layout: like examples/purv.ld (code in the read-only lower half at 0,
 * writable data in the upper half at 0x80000000), EXCEPT .text and .rodata are kept
 * as separate output sections instead of merged into one.
 *
 * The image builder (transcode) needs an exact, non-heuristic split between
 * instructions and read-only data -- it transcodes ONLY .text into ops; .rodata
 * stays raw bytes. Two adjacent sections with their own section-header sizes give
 * it that split directly (no scanning for "where code ends"). Both still live in
 * the same read-only lower half, so the address layout purva runs is unchanged:
 * .rodata is placed immediately after .text. */
SECTIONS {
  . = 0x00000000;
  .text : {
    *(.text._start)
    *(.text .text.*)
  }
  .rodata : {
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
