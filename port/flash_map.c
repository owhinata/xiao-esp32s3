/*
 * Map the flash-resident parts of the image (code and read-only data) into
 * the address space.  Runs from RAM, before anything in flash is reachable.
 *
 * This is the part of the job that ESP-IDF's second-stage bootloader does for
 * an application (bootloader_init_spi_flash() and set_cache_and_start_app()
 * in components/bootloader_support, Apache-2.0); the register sequences below
 * follow that code.  The cache and MMU themselves are driven through the HAL.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_rom_efuse.h"
#include "esp_rom_gpio.h"
#include "esp_rom_spiflash.h"
#include "esp_rom_sys.h"
#include "esp32s3/rom/cache.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "hal/mmu_hal.h"
#include "hal/mmu_types.h"
#include "soc/soc.h"
#include "soc/spi_mem_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/spi_pins.h"

#include "board.h"

extern char _flash_text_start, _flash_text_end;
extern char _flash_rodata_start, _flash_rodata_end;

/* ROM functions without a declaration in the esp_rom headers. */
extern void rom_config_instruction_cache_mode(uint32_t size, uint8_t ways, uint8_t line_size);
extern void rom_config_data_cache_mode(uint32_t size, uint8_t ways, uint8_t line_size);

/*
 * Cache geometry: ESP-IDF's defaults for the ESP32-S3.  The caches live in
 * internal SRAM: the instruction cache in the first bytes of SRAM0, which the
 * linker script therefore leaves empty (ICACHE_SIZE there is the single source
 * of the size), the data cache at the top of SRAM2.
 */
extern char _icache_size;       /* absolute symbol, value = size in bytes */
#define BOARD_ICACHE_SIZE       ((uint32_t) &_icache_size)
#define BOARD_DCACHE_SIZE       0x8000u     /* 32 KiB */
#define BOARD_CACHE_WAYS        8u
#define BOARD_CACHE_LINE_SIZE   32u

#define IMAGE_MAGIC             0xE9u
#define IMAGE_HEADER_SIZE       24u     /* common + extended header */
#define SEGMENT_HEADER_SIZE     8u
#define IMAGE_DIGEST_SIZE       32u
#define MAX_TRAILING_SEGMENTS   8u      /* padding + flash segments behind the RAM ones */

static void __attribute__((noreturn)) fatal(const char *what)
{
    /* The console is not up yet; this goes to the ROM's own output channels. */
    esp_rom_printf("\nboot: %s\n", what);
    for (;;) {
    }
}

static void flash_read(uint32_t offset, uint32_t *dest, uint32_t len)
{
    if (esp_rom_spiflash_read(offset, dest, (int32_t) len) != ESP_ROM_SPIFLASH_RESULT_OK) {
        fatal("flash read failed");
    }
}

/*
 * Find where esptool put the two flash segments.
 *
 * Layout written by "elf2image --ram-only-header":
 *
 *   image header, with segment count = number of RAM segments
 *   RAM segments                        (8-byte header + data each)
 *   checksum byte, last byte of a 16-byte block
 *   SHA-256 of everything up to here   (added by script/add_boot_digest.py;
 *                                       the ROM checks it)
 *   per flash segment: one padding segment (load address 0), then the segment,
 *   placed so that its data starts at a file offset congruent to its load
 *   address modulo the MMU page size.
 *
 * The image is flashed at offset 0, so file offsets are flash offsets.
 */
static void find_flash_segments(uint32_t text_vaddr, uint32_t rodata_vaddr,
                                uint32_t *text_offset, uint32_t *rodata_offset)
{
    uint32_t header[IMAGE_HEADER_SIZE / 4];
    uint32_t seg[2];            /* load address, length */
    uint32_t offset;
    uint32_t i;
    bool have_text = false;
    bool have_rodata = false;

    flash_read(0, header, sizeof(header));
    if ((header[0] & 0xFFu) != IMAGE_MAGIC) {
        fatal("no image header at flash offset 0");
    }

    offset = IMAGE_HEADER_SIZE;
    for (i = (header[0] >> 8) & 0xFFu; i > 0; i--) {
        flash_read(offset, seg, sizeof(seg));
        offset += SEGMENT_HEADER_SIZE + seg[1];
    }
    offset = ((offset + 16u) & ~15u) + IMAGE_DIGEST_SIZE;

    for (i = 0; i < MAX_TRAILING_SEGMENTS && !(have_text && have_rodata); i++) {
        flash_read(offset, seg, sizeof(seg));
        if (seg[0] == text_vaddr) {
            *text_offset = offset + SEGMENT_HEADER_SIZE;
            have_text = true;
        } else if (seg[0] == rodata_vaddr) {
            *rodata_offset = offset + SEGMENT_HEADER_SIZE;
            have_rodata = true;
        } else if (seg[0] != 0) {
            break;              /* neither padding nor one of ours */
        }
        offset += SEGMENT_HEADER_SIZE + seg[1];
    }

    if (!have_text || !have_rodata) {
        fatal("flash segments not found in the image (built without --ram-only-header?)");
    }
}

/* Pad drive strength, dummy-phase line levels and chip-select timing. */
static void flash_pins_and_timing(void)
{
    const uint32_t spiconfig = esp_rom_efuse_get_flash_gpio_info();
    uint32_t pins[6] = {
        MSPI_IOMUX_PIN_NUM_CLK, MSPI_IOMUX_PIN_NUM_MISO, MSPI_IOMUX_PIN_NUM_MOSI,
        MSPI_IOMUX_PIN_NUM_CS0, MSPI_IOMUX_PIN_NUM_HD, MSPI_IOMUX_PIN_NUM_WP,
    };
    uint32_t i;

    if (spiconfig != 0) {       /* pins remapped by eFuse */
        pins[0] = spiconfig & 0x3Fu;
        pins[1] = (spiconfig >> 6) & 0x3Fu;
        pins[2] = (spiconfig >> 12) & 0x3Fu;
        pins[3] = (spiconfig >> 18) & 0x3Fu;
        pins[4] = (spiconfig >> 24) & 0x3Fu;
        pins[5] = esp_rom_efuse_get_flash_wp_gpio();
    }
    for (i = 0; i < 6; i++) {
        if (pins[i] <= MAX_PAD_GPIO_NUM) {
            esp_rom_gpio_pad_set_drv(pins[i], 1);
        }
    }

    REG_SET_BIT(SPI_MEM_CTRL_REG(0), SPI_MEM_FDUMMY_OUT | SPI_MEM_D_POL | SPI_MEM_Q_POL);
    REG_SET_BIT(SPI_MEM_CTRL_REG(1), SPI_MEM_FDUMMY_OUT | SPI_MEM_D_POL | SPI_MEM_Q_POL);

    /* SPI0 and SPI1 share these chip-select settings. */
    SET_PERI_REG_BITS(SPI_MEM_CTRL2_REG(0), SPI_MEM_CS_HOLD_TIME_V, 0, SPI_MEM_CS_HOLD_TIME_S);
    SET_PERI_REG_BITS(SPI_MEM_CTRL2_REG(0), SPI_MEM_CS_SETUP_TIME_V, 0, SPI_MEM_CS_SETUP_TIME_S);
    SET_PERI_REG_MASK(SPI_MEM_USER_REG(0), SPI_MEM_CS_HOLD_M | SPI_MEM_CS_SETUP_M);
}

static void map_segment(uint32_t vaddr, uint32_t flash_offset, uint32_t size)
{
    const uint32_t page_mask = ~((uint32_t) CONFIG_MMU_PAGE_SIZE - 1u);
    const uint32_t vaddr_page = vaddr & page_mask;
    uint32_t mapped = 0;

    if ((vaddr & ~page_mask) != (flash_offset & ~page_mask)) {
        fatal("flash segment is not page-congruent with its address");
    }
    size += vaddr - vaddr_page;
    mmu_hal_map_region(0, MMU_TARGET_FLASH0, vaddr_page, flash_offset & page_mask, size, &mapped);
    cache_ll_l1_enable_bus(0, cache_ll_l1_get_bus(0, vaddr_page, size));
}

void flash_map_image(void)
{
    const uint32_t text_vaddr   = (uint32_t) &_flash_text_start;
    const uint32_t rodata_vaddr = (uint32_t) &_flash_rodata_start;
    uint32_t text_offset = 0;
    uint32_t rodata_offset = 0;

    find_flash_segments(text_vaddr, rodata_vaddr, &text_offset, &rodata_offset);

    cache_hal_init();
    mmu_hal_init();
    flash_pins_and_timing();

    cache_hal_disable(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);
    mmu_hal_unmap_all();
    map_segment(rodata_vaddr, rodata_offset, (uint32_t) (&_flash_rodata_end - &_flash_rodata_start));
    map_segment(text_vaddr, text_offset, (uint32_t) (&_flash_text_end - &_flash_text_start));

    cache_hal_enable(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);

    /*
     * What ESP-IDF's application start-up (call_start_cpu0) adds on top of
     * the bootloader's mapping.  The bootloader only ever reads flash as
     * data; executing from it needs both of these.
     *
     * 1. Cache geometry.  The values are ESP-IDF's defaults.
     */
    rom_config_instruction_cache_mode(BOARD_ICACHE_SIZE, BOARD_CACHE_WAYS, BOARD_CACHE_LINE_SIZE);
    Cache_Suspend_DCache();
    rom_config_data_cache_mode(BOARD_DCACHE_SIZE, BOARD_CACHE_WAYS, BOARD_CACHE_LINE_SIZE);
    Cache_Resume_DCache(0);

    /*
     * 2. The instruction and data windows share one MMU table, and the ROM's
     *    cache code has to be told where the entries used for code end and
     *    the ones used for data begin (ESP_ROM_NEEDS_SET_CACHE_MMU_SIZE).  The
     *    linker script puts read-only data on the first page after the code,
     *    so that page number is the boundary.
     */
    {
        const uint32_t irom_entries_size =
            ((rodata_vaddr & ~((uint32_t) CONFIG_MMU_PAGE_SIZE - 1u)) - SOC_DROM_LOW)
            / CONFIG_MMU_PAGE_SIZE * sizeof(uint32_t);

        Cache_Set_IDROM_MMU_Size(irom_entries_size, CACHE_DROM_MMU_MAX_END - irom_entries_size);
    }
}