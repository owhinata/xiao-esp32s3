/*
 * XIAO ESP32S3 -- RAM-only image.
 *
 * The first-stage ROM bootloader copies every segment of the image into
 * internal SRAM and jumps to _start.  Nothing executes from flash, so no
 * second-stage bootloader, cache or MMU setup is involved.
 *
 * Internal SRAM as seen by the two buses:
 *
 *   SRAM0   32 KiB   0x40370000-0x40378000 (instruction bus only)
 *   SRAM1  416 KiB   0x40378000-0x403E0000 (instruction bus)
 *                    0x3FC88000-0x3FCF0000 (data bus, same memory)
 *
 * SRAM1 is one memory with two addresses.  This script gives its low 96 KiB
 * to code and the rest to data; the split point below must name the same
 * physical byte on both buses (instruction address - 0x6F0000 = data address).
 *
 * The top of SRAM1 from 0x3FCE9710 up belongs to the ROM bootloader while it
 * loads the image (its stack and static data), so no loaded section may land
 * there.  _start keeps running on that ROM stack until the first ThreadX
 * thread is scheduled.
 */

ENTRY(_start)

IRAM_ORG        = 0x40370000;
IRAM_END        = 0x40390000;                   /* split point, instruction bus */
DRAM_ORG        = IRAM_END - 0x6F0000;          /* split point, data bus = 0x3FCA0000 */
DRAM_END        = 0x3FCE9710;                   /* start of ROM bootloader stack/data */

MEMORY
{
    iram_seg (RX) : org = 0x40370000, len = 0x40390000 - 0x40370000
    dram_seg (RW) : org = 0x3FCA0000, len = 0x3FCE9710 - 0x3FCA0000
}

ASSERT(ORIGIN(iram_seg) == IRAM_ORG && ORIGIN(iram_seg) + LENGTH(iram_seg) == IRAM_END,
       "iram_seg does not match IRAM_ORG / IRAM_END")
ASSERT(ORIGIN(dram_seg) == DRAM_ORG && ORIGIN(dram_seg) + LENGTH(dram_seg) == DRAM_END,
       "dram_seg does not start at the data-bus alias of IRAM_END")

SECTIONS
{
    /*
     * Exception and interrupt vectors (esp-hal-components xtensa_vectors.S).
     * VECBASE is set to _vector_table by _start; the offsets are fixed by the
     * ESP32-S3 core configuration (XCHAL_*_VECOFS in xtensa/config/core-isa.h).
     *
     * Literals belonging to the vectors go first: l32r can only reach
     * backwards, so they must sit below the code that loads them.
     */
    .iram0.vectors :
    {
        . = ALIGN(4);
        *(.*Vector.literal)
        . = ALIGN(0x400);
        _vector_table = ABSOLUTE(.);
        _vector_start = .;
        . = _vector_start + 0x000;  KEEP(*(.WindowVectors.text))
        . = _vector_start + 0x180;  KEEP(*(.Level2InterruptVector.text))
        . = _vector_start + 0x1C0;  KEEP(*(.Level3InterruptVector.text))
        . = _vector_start + 0x200;  KEEP(*(.Level4InterruptVector.text))
        . = _vector_start + 0x240;  KEEP(*(.Level5InterruptVector.text))
        . = _vector_start + 0x280;  KEEP(*(.DebugExceptionVector.text))
        . = _vector_start + 0x2C0;  KEEP(*(.NMIExceptionVector.text))
        . = _vector_start + 0x300;  KEEP(*(.KernelExceptionVector.text))
        . = _vector_start + 0x340;  KEEP(*(.UserExceptionVector.text))
        . = _vector_start + 0x3C0;  KEEP(*(.DoubleExceptionVector.text))
        . = _vector_start + 0x400;
        /* Where backtraces of freshly created contexts end; the layer wants it here. */
        KEEP(*(._invalid_pc_placeholder.text))
    } > iram_seg

    .iram0.text :
    {
        . = ALIGN(4);
        _text_start = ABSOLUTE(.);
        *(.entry.literal .entry.text)
        *(.iram1.literal .iram1 .iram1.*)       /* Espressif's "must be in RAM" code */
        *(.literal .text .literal.* .text.*)
        *(.stub .gnu.warning .gnu.linkonce.literal.* .gnu.linkonce.t.*)
        *(.fini.literal .fini .init.literal .init)
        . = ALIGN(4);
        _text_end = ABSOLUTE(.);
    } > iram_seg

    .dram0.rodata :
    {
        . = ALIGN(4);
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        *(.rodata1)
        . = ALIGN(4);
        __preinit_array_start = ABSOLUTE(.);
        KEEP(*(.preinit_array))
        __preinit_array_end = ABSOLUTE(.);
        __init_array_start = ABSOLUTE(.);
        KEEP(*(SORT(.init_array.*) .init_array))
        __init_array_end = ABSOLUTE(.);
        . = ALIGN(4);
    } > dram_seg

    .dram0.data :
    {
        . = ALIGN(4);
        *(.data .data.* .gnu.linkonce.d.*)
        *(.data1)
        *(.sdata .sdata.* .gnu.linkonce.s.*)
        *(.sdata2 .sdata2.* .gnu.linkonce.s2.*)
        . = ALIGN(4);
    } > dram_seg

    .dram0.bss (NOLOAD) :
    {
        . = ALIGN(8);
        _bss_start = ABSOLUTE(.);
        *(.dynsbss .sbss .sbss.* .gnu.linkonce.sb.*)
        *(.scommon .sbss2 .sbss2.* .gnu.linkonce.sb2.*)
        *(.dynbss .bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = ABSOLUTE(.);
    } > dram_seg

    /*
     * _end   : first free byte.  ThreadX hands it to tx_application_define().
     * __stack: end of usable data RAM.  The ThreadX port puts the C library
     *          heap in the upper half of [_end, __stack) by default.
     */
    . = ALIGN(16);
    _end    = ABSOLUTE(.);
    __stack = ABSOLUTE(DRAM_END);

    /DISCARD/ :
    {
        *(.xtensa.info)
        *(.comment)
        *(.note.GNU-stack)
    }
}
