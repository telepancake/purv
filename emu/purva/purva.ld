/* purva guest layout: matches purv.h's documented four-region model exactly --
 * code and rodata are two SEPARATE regions within the read-only lower half, not
 * one merged blob (unlike examples/purv.ld, which purv's own drivers use, and
 * can get away with because purv's "code" bytes stay real RISC-V instructions
 * forever: reading them as data is meaningless but harmless. purva's "code" is
 * transcoded, packed op words -- there is nothing sane to read there, and its
 * byte length isn't even stable across a fusion pass, so code must never be
 * data-addressable at all; see transcode.h and purva.c's mem_xlate).
 *
 * .text grows up from 0 (the code window). .rodata grows DOWN from 0x80000000,
 * exactly like purv.h documents region[RISCV_RODATA] -- placed by address, not
 * by assumption: `. = 0x80000000 - SIZEOF(.rodata)` is a real, supported ld.lld
 * idiom (verified: the linker computes a section's total input size before
 * resolving address expressions that reference it). This is what makes rodata's
 * real address independent of anything about .text's size: two architectural
 * fixed points (0 and 0x80000000) anchor the two regions from opposite ends,
 * with the whole rest of the lower half as slack between them -- no separate
 * split constant to pick, no risk of code ever growing into it. tctool.c reads
 * the resulting real address back from the ELF (MERGE_AUTO_BASE) rather than
 * assuming it, so this works for any other purva linker script too, not only
 * this one. */
SECTIONS {
  . = 0x00000000;
  .text : {
    *(.text._start)
    *(.text .text.*)
  }

  . = 0x80000000 - SIZEOF(.rodata);
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
