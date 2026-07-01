/* purva guest layout: matches purv.h's two-cluster model. The read-only cluster
 * sits AROUND 0 -- rodata just below it (small negative addresses), code from 0 up
 * -- but for purva code and rodata stay two SEPARATE regions (unlike examples/
 * purv.ld, which purv's own drivers use, and can get away with because purv's
 * "code" bytes stay real RISC-V instructions forever: reading them as data is
 * meaningless but harmless. purva's "code" is transcoded, packed op words -- there
 * is nothing sane to read there, and its byte length isn't even stable across a
 * fusion pass, so code must never be data-addressable at all; see transcode.h and
 * purva.c's mem_xlate).
 *
 * .text grows up from 0 (the code window). .rodata grows DOWN from 0 so it ends
 * EXACTLY at 2^32 and lives at small negative addresses, exactly like purv.h
 * documents the read-only cluster -- placed by address, not by assumption:
 * `. = 0x100000000 - SIZEOF(.rodata)` is a real, supported ld.lld idiom (verified:
 * the linker computes a section's total input size before resolving address
 * expressions that reference it, and a vaddr up to 0xffffffff is fine in a 32-bit
 * output). This makes rodata's real address independent of anything about .text's
 * size: two architectural fixed points (0 for code, 2^32 for rodata's top) anchor
 * the read-only cluster from both ends. tctool.c reads the resulting real address
 * back from the ELF (MERGE_AUTO_BASE) rather than assuming it, so this works for
 * any other purva linker script too, not only this one. */
SECTIONS {
  . = 0x00000000;
  .text : {
    *(.text._start)
    *(.text .text.*)
  }

  . = 0x100000000 - SIZEOF(.rodata);
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
