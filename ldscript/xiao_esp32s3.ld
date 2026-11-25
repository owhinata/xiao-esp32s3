/*
 * XIAO ESP32S3 -- application image, booted by ESP-IDF's second-stage
 * bootloader and executing from flash.
 *
 * The image is a standard ESP-IDF application image ("esptool elf2image"):
 * the bootloader (bootloader/) finds it in the "factory" partition, checks
 * its SHA-256, copies the RAM segments into SRAM, maps the two flash segments
 * through the cache and MMU, and jumps to _start (port/startup.c).
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
 * What the boot chain occupies at the top of SRAM1 (data-bus addresses), from
 * the bootloader's linker script (bootloader/subproject/main/ld/esp32s3/
 * bootloader.memory.ld.in) and ESP-IDF's esp_system/ld/esp32s3/memory.ld.in:
 *
 *   0x3FCD7E00-0x3FCE9704  ROM: buffers of the download mode (bootsel)
 *   0x3FCD8700-0x3FCDB700  bootloader code that is done before the
 *                          application is loaded (its "iram_seg")
 *   0x3FCDB700-0x3FCE2700  bootloader code that loads the application
 *                          ("iram_loader_seg")
 *   0x3FCE2700-0x3FCE7700  bootloader data
 *   0x3FCE7700-0x3FCEB710  stack: the ROM's start-up stack from 0x3FCEB710
 *                          down, which the bootloader and _start run on
 *   0x3FCED710-0x3FCF0000  ROM data
 *
 * The bootloader refuses to load a segment that overlaps its loader code,
 * its data or its stack, so everything loaded ends below 0x3FCDB700
 * (DRAM_END, ESP-IDF's SRAM_DRAM_END).  Once _start runs, none of this is
 * needed any more: the heap may use everything up to the bottom of the ROM's
 * stack area, 0x3FCE9710 (HEAP_END, ESP-IDF's APP_USABLE_DRAM_END), which
 * stays clear of the stack _start and tx_application_define() run on.
 *
 * .dram0.noinit (the RAM log) keeps its contents across a reset only if
 * nothing writes there between two runs.  The bootloader writes only its own
 * sections, its stack and the application's loaded segments, and clears only
 * its own .bss; the ROM writes its download-mode buffers when bootsel is
 * used.  .dram0.noinit is therefore kept below 0x3FCD7E00 (NOINIT_END).
 *
 * The two flash windows share one MMU table: page N of the instruction window
 * and page N of the data window are the same table entry.  Code starts on the
 * first page of the instruction window; read-only data is placed in the data
 * window on the pages that follow the ones the code occupies.  The flash
 * address of a segment and its load address must agree modulo the 64 KiB MMU
 * page: esptool puts the data segment first, right behind the 0x18-byte image
 * header and its own 0x8-byte segment header, so it starts 0x20 into a page;
 * for the code segment esptool inserts padding.  The bootloader reads the
 * application description (esp_app_desc_t) from the start of that first
 * segment.
 */

ENTRY(_start)

ICACHE_SIZE     = 0x4000;                       /* 16 KiB; port/startup.c programs the cache with it */
IRAM_ORG        = 0x40370000 + ICACHE_SIZE;
IRAM_END        = 0x40390000;                   /* split point, instruction bus */
DRAM_ORG        = IRAM_END - 0x6F0000;          /* split point, data bus = 0x3FCA0000 */
DRAM_END        = 0x3FCDB700;                   /* start of the bootloader's loader code */
NOINIT_END      = 0x3FCD7E00;                   /* start of the ROM's download-mode buffers */
HEAP_END        = 0x3FCE9710;                   /* bottom of the ROM's start-up stack area */
MMU_PAGE        = 0x10000;
IMAGE_HEADERS   = 0x20;                         /* image header + first segment header */

MEMORY
{
    iram_seg (RX) : org = 0x40374000, len = 0x40390000 - 0x40374000
    dram_seg (RW) : org = 0x3FCA0000, len = 0x3FCDB700 - 0x3FCA0000
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
     * Exception and interrupt vectors (esp-hal-3rdparty xtensa_vectors.S).
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
     *   - the start-up code, which reconfigures the cache that code in flash
     *     is fetched through (port/startup.c), and the ROM cache patches it
     *     calls (esp_rom_cache_esp32s2_esp32s3.c),
     *   - fault reporting, which has to work when the flash mapping is the
     *     thing that is broken,
     *   - what Espressif marks IRAM_ATTR (.iram1), and the objects ESP-IDF
     *     keeps out of flash in an application so that they work with the
     *     cache disabled: the clock code with its analog-register access
     *     (rtc_clk.c, RTC_CLK_FUNC_IN_IRAM; regi2c_ctrl.c) and the watchdog
     *     HAL (wdt_hal_iram.c),
     *   - interrupt entry/exit and context switch, with the execution profile
     *     kit they call (tx_execution_profile.c).
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
        *rtc_clk.c.obj(.literal .literal.* .text .text.*)
        *regi2c_ctrl.c.obj(.literal .literal.* .text .text.*)
        *wdt_hal_iram.c.obj(.literal .literal.* .text .text.*)
        *esp_rom_cache_esp32s2_esp32s3.c.obj(.literal .literal.* .text .text.*)
        *xtensa_context.S.obj(.literal .literal.* .text .text.*)
        *xtensa_intr_asm.S.obj(.literal .literal.* .text .text.*)
        *tx_*.S.obj(.literal .literal.* .text .text.*)
        *tx_execution_profile.c.obj(.literal .literal.* .text .text.*)
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
        /* Read-only data of the objects that run from RAM (see .iram0.text). */
        *startup.c.obj(.rodata .rodata.*)
        *panic.c.obj(.rodata .rodata.*)
        *rtc_clk.c.obj(.rodata .rodata.*)
        *regi2c_ctrl.c.obj(.rodata .rodata.*)
        *wdt_hal_iram.c.obj(.rodata .rodata.*)
        *esp_rom_cache_esp32s2_esp32s3.c.obj(.rodata .rodata.*)
        *xtensa_context.S.obj(.rodata .rodata.*)
        *xtensa_intr_asm.S.obj(.rodata .rodata.*)
        *xtensa_vectors.S.obj(.rodata .rodata.*)
        *tx_*.S.obj(.rodata .rodata.*)
        *tx_execution_profile.c.obj(.rodata .rodata.*)
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
     * across a reset that leaves the RAM powered (the RAM log).  It has to stay
     * below what the ROM and the bootloader write (NOINIT_END, see the top).
     */
    .dram0.noinit (NOLOAD) :
    {
        . = ALIGN(8);
        *(.noinit .noinit.*)
        . = ALIGN(8);
    } > dram_seg

    ASSERT(ADDR(.dram0.noinit) + SIZEOF(.dram0.noinit) <= NOINIT_END,
           ".dram0.noinit reaches memory the ROM or the bootloader writes during a reset")

    /*
     * _end   : first free byte.  ThreadX hands it to tx_application_define().
     * __stack: end of usable data RAM (HEAP_END).  The ThreadX port puts the C
     *          library heap in the upper half of [_end, __stack) by default.
     */
    . = ALIGN(16);
    _end    = ABSOLUTE(.);
    __stack = ABSOLUTE(HEAP_END);

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

    /*
     * First MMU page after the ones .flash.text occupies, in the data window,
     * 0x20 bytes in: where the first segment of the image file starts.
     */
    _flash_rodata_vma = ORIGIN(drom_seg) + ALIGN(_flash_text_end - ORIGIN(irom_seg), MMU_PAGE)
                        + IMAGE_HEADERS;

    .flash.rodata _flash_rodata_vma :
    {
        _flash_rodata_start = ABSOLUTE(.);
        /* esp_app_desc_t, first: the bootloader and esptool look for it here. */
        KEEP(*(.rodata_desc .rodata_desc.*))
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

    ASSERT(esp_app_desc == _flash_rodata_start,
           "esp_app_desc must be the first thing in .flash.rodata")

    /DISCARD/ :
    {
        *(.xtensa.info)
        *(.comment)
        *(.note.GNU-stack)
    }
}
