/*
 * XIAO ESP32S3 -- image booted directly by the ROM, executing from flash.
 *
 * The image is built with "esptool elf2image --ram-only-header": the ROM
 * bootloader sees and loads only the RAM segments, then jumps to _start.
 * _start (port/startup.c, port/flash_map.c) sets up the cache and MMU and
 * maps the two flash segments, which sit behind the RAM segments in the same
 * image.  There is no second-stage bootloader.
 *
 * Address spaces:
 *
 *   SRAM0   32 KiB   0x40370000-0x40378000 (instruction bus only).  Its first
 *                    ICACHE_SIZE bytes are the instruction cache's own memory
 *                    and must stay unused.
 *   SRAM1  416 KiB   0x40378000-0x403E0000 (instruction bus)
 *                    0x3FC88000-0x3FCF0000 (data bus, same memory)
 *   flash, through the cache and MMU in 64 KiB pages:
 *                    0x42000000-0x44000000 (instruction bus)
 *                    0x3C000000-0x3E000000 (data bus)
 *
 * (The data cache takes its memory from the top of SRAM2, 0x3FCF0000-
 * 0x3FD00000, which this script does not use.)
 *
 * SRAM1 is one memory with two addresses.  This script gives its low 96 KiB
 * to code and the rest to data; the split point below must name the same
 * physical byte on both buses (instruction address - 0x6F0000 = data address).
 *
 * The top of SRAM1 from 0x3FCE9710 up belongs to the ROM bootloader while it
 * loads the image (its stack and static data), so no loaded section may land
 * there.  _start keeps running on that ROM stack until the first ThreadX
 * thread is scheduled.
 *
 * The two flash windows share one MMU table: page N of the instruction window
 * and page N of the data window are the same table entry.  Read-only data is
 * therefore placed in the data window at the pages that follow the ones the
 * code occupies in the instruction window.
 */

ENTRY(_start)

ICACHE_SIZE     = 0x4000;                       /* 16 KiB; port/flash_map.c programs the cache with it */
IRAM_ORG        = 0x40370000 + ICACHE_SIZE;
IRAM_END        = 0x40390000;                   /* split point, instruction bus */
DRAM_ORG        = IRAM_END - 0x6F0000;          /* split point, data bus = 0x3FCA0000 */
DRAM_END        = 0x3FCE9710;                   /* start of ROM bootloader stack/data */
MMU_PAGE        = 0x10000;

MEMORY
{
    iram_seg (RX) : org = 0x40374000, len = 0x40390000 - 0x40374000
    dram_seg (RW) : org = 0x3FCA0000, len = 0x3FCE9710 - 0x3FCA0000
    irom_seg (RX) : org = 0x42000000, len = 0x800000
    drom_seg (R)  : org = 0x3C000000, len = 0x800000
}

ASSERT(ORIGIN(iram_seg) == IRAM_ORG && ORIGIN(iram_seg) + LENGTH(iram_seg) == IRAM_END,
       "iram_seg does not match IRAM_ORG / IRAM_END")
ASSERT(ORIGIN(dram_seg) == DRAM_ORG && ORIGIN(dram_seg) + LENGTH(dram_seg) == DRAM_END,
       "dram_seg does not start at the data-bus alias of IRAM_END")

_icache_size = ABSOLUTE(ICACHE_SIZE);

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

    /*
     * Code that must run from RAM:
     *   - everything executed before the flash is mapped (reset entry, clock
     *     and cache set-up and what they call),
     *   - fault reporting, which has to work when the flash mapping is the
     *     thing that is broken,
     *   - what Espressif marks IRAM_ATTR (.iram1),
     *   - interrupt entry/exit and context switch.
     * The same list appears in .dram0.data for the read-only data of these
     * objects.  Keep the two in step.
     */
    .iram0.text :
    {
        . = ALIGN(4);
        _iram_text_start = ABSOLUTE(.);
        *(.entry.literal .entry.text)
        *(.iram1.literal .iram1 .iram1.*)
        *startup.c.obj(.literal .literal.* .text .text.*)
        *panic.c.obj(.literal .literal.* .text .text.*)
        *flash_map.c.obj(.literal .literal.* .text .text.*)
        *esp_stubs.c.obj(.literal .literal.* .text .text.*)
        *rtc_clk.c.obj(.literal .literal.* .text .text.*)
        *rtc_clk_init.c.obj(.literal .literal.* .text .text.*)
        *rtc_init.c.obj(.literal .literal.* .text .text.*)
        *regi2c_ctrl.c.obj(.literal .literal.* .text .text.*)
        *wdt_hal_iram.c.obj(.literal .literal.* .text .text.*)
        *cache_hal.c.obj(.literal .literal.* .text .text.*)
        *mmu_hal.c.obj(.literal .literal.* .text .text.*)
        *esp_rom_cache_esp32s2_esp32s3.c.obj(.literal .literal.* .text .text.*)
        *xtensa_context.S.obj(.literal .literal.* .text .text.*)
        *xtensa_intr_asm.S.obj(.literal .literal.* .text .text.*)
        *tx_*.S.obj(.literal .literal.* .text .text.*)
        *libxt_hal.a:(.literal .literal.* .text .text.*)
        . = ALIGN(4);
        _iram_text_end = ABSOLUTE(.);
    } > iram_seg

    .dram0.data :
    {
        . = ALIGN(4);
        *(.data .data.* .gnu.linkonce.d.*)
        *(.data1)
        *(.sdata .sdata.* .gnu.linkonce.s.*)
        *(.sdata2 .sdata2.* .gnu.linkonce.s2.*)
        /* Read-only data of the objects that run before the flash is mapped. */
        *startup.c.obj(.rodata .rodata.*)
        *panic.c.obj(.rodata .rodata.*)
        *flash_map.c.obj(.rodata .rodata.*)
        *esp_stubs.c.obj(.rodata .rodata.*)
        *rtc_clk.c.obj(.rodata .rodata.*)
        *rtc_clk_init.c.obj(.rodata .rodata.*)
        *rtc_init.c.obj(.rodata .rodata.*)
        *regi2c_ctrl.c.obj(.rodata .rodata.*)
        *wdt_hal_iram.c.obj(.rodata .rodata.*)
        *cache_hal.c.obj(.rodata .rodata.*)
        *mmu_hal.c.obj(.rodata .rodata.*)
        *esp_rom_cache_esp32s2_esp32s3.c.obj(.rodata .rodata.*)
        *xtensa_context.S.obj(.rodata .rodata.*)
        *xtensa_intr_asm.S.obj(.rodata .rodata.*)
        *xtensa_vectors.S.obj(.rodata .rodata.*)
        *tx_*.S.obj(.rodata .rodata.*)
        *libxt_hal.a:(.rodata .rodata.*)
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
     * Not loaded and not cleared by _start: what is here keeps its contents
     * across a reset that leaves the RAM powered (the RAM log).
     */
    .dram0.noinit (NOLOAD) :
    {
        . = ALIGN(8);
        *(.noinit .noinit.*)
        . = ALIGN(8);
    } > dram_seg

    /*
     * _end   : first free byte.  ThreadX hands it to tx_application_define().
     * __stack: end of usable data RAM.  The ThreadX port puts the C library
     *          heap in the upper half of [_end, __stack) by default.
     */
    . = ALIGN(16);
    _end    = ABSOLUTE(.);
    __stack = ABSOLUTE(DRAM_END);

    /* Everything else executes in place from flash. */
    .flash.text :
    {
        _flash_text_start = ABSOLUTE(.);
        *(.literal .text .literal.* .text.*)
        *(.stub .gnu.warning .gnu.linkonce.literal.* .gnu.linkonce.t.*)
        *(.fini.literal .fini .init.literal .init)
        . = ALIGN(4);
        _flash_text_end = ABSOLUTE(.);
    } > irom_seg

    /* First MMU page after the ones .flash.text occupies, in the data window. */
    _flash_rodata_vma = ORIGIN(drom_seg) + ALIGN(_flash_text_end - ORIGIN(irom_seg), MMU_PAGE);

    .flash.rodata _flash_rodata_vma :
    {
        _flash_rodata_start = ABSOLUTE(.);
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
        _flash_rodata_end = ABSOLUTE(.);
    } > drom_seg

    /DISCARD/ :
    {
        *(.xtensa.info)
        *(.comment)
        *(.note.GNU-stack)
    }
}
