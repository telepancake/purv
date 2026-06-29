/* Minimal layout for a fixed-address, statically-linked RV32 ELF.
 *
 * One PT_LOAD segment that *includes the ELF header and program headers*
 * (FILEHDR PHDRS), so there is no padding between the headers and the code --
 * the file is just [ehdr][phdr][text+rodata+data][ (bss is NOBITS) ]. The whole
 * image loads at RAM_ORIGIN; entry is _start, a few dozen bytes in. */
ENTRY(_start)

PHDRS {
  image PT_LOAD FILEHDR PHDRS FLAGS(7);   /* RWX: one segment for everything */
}

SECTIONS {
  . = 0x80000000 + SIZEOF_HEADERS;

  .text : {
    *(.text._start)
    *(.text .text.*)
    *(.rodata .rodata.* .srodata .srodata.*)
    *(.data .data.* .sdata .sdata.*)
  } :image

  .bss (NOLOAD) : {
    *(.sbss .sbss.* .bss .bss.* COMMON)
  } :image

  /DISCARD/ : {
    *(.comment) *(.note .note.*) *(.riscv.attributes)
    *(.eh_frame) *(.eh_frame_hdr)
  }
}
