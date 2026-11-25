/*
 * XIAO ESP32S3 board support: LED, CPU clock readback, polled console.
 */
#include <stdint.h>

#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "soc/rtc.h"
#include "soc/soc.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "hal/gpio_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include "board.h"

/* ---- LED ----------------------------------------------------------------- */

void board_led_init(void)
{
    gpio_ll_func_sel(&GPIO, BOARD_LED_GPIO, PIN_FUNC_GPIO);
    gpio_ll_matrix_out_default(&GPIO, BOARD_LED_GPIO);
    gpio_ll_od_disable(&GPIO, BOARD_LED_GPIO);
    board_led_set(0);
    gpio_ll_output_enable(&GPIO, BOARD_LED_GPIO);
}

void board_led_set(int on)
{
    gpio_ll_set_level(&GPIO, BOARD_LED_GPIO,
                      on ? BOARD_LED_ACTIVE_LEVEL : !BOARD_LED_ACTIVE_LEVEL);
}

/* ---- CPU clock ----------------------------------------------------------- */

/*
 * This image does not program the clock tree; it runs at whatever the ROM
 * bootloader selected.  Read the selection back rather than assume it, since
 * the ThreadX tick is derived from this number.
 */
uint32_t board_cpu_freq_hz(void)
{
    rtc_cpu_freq_config_t config;

    rtc_clk_cpu_freq_get_config(&config);
    return config.freq_mhz * 1000000u;
}

/* ---- Console ------------------------------------------------------------- */

/*
 * How long to wait for room in the USB TX FIFO before concluding that no
 * host is reading.  Once that happens output is dropped without waiting until
 * the FIFO drains again, so a missing terminal costs one timeout, not one per
 * character.
 */
#define CONSOLE_TX_TIMEOUT_MS   50u

static uint8_t console_host_stalled;

static int console_wait_writable(void)
{
    uint32_t start;
    uint32_t limit;

    if (usb_serial_jtag_ll_txfifo_writable()) {
        console_host_stalled = 0;
        return 1;
    }
    if (console_host_stalled) {
        return 0;
    }

    limit = esp_rom_get_cpu_ticks_per_us() * 1000u * CONSOLE_TX_TIMEOUT_MS;
    start = esp_cpu_get_cycle_count();
    while (!usb_serial_jtag_ll_txfifo_writable()) {
        if ((uint32_t) (esp_cpu_get_cycle_count() - start) > limit) {
            console_host_stalled = 1;
            return 0;
        }
    }
    return 1;
}

static void console_write_noflush(const char *buf, uint32_t len)
{
    while (len > 0) {
        uint32_t n;

        if (!console_wait_writable()) {
            return;
        }
        n = usb_serial_jtag_ll_write_txfifo((const uint8_t *) buf, len);
        buf += n;
        len -= n;
    }
}

void console_write(const char *buf, uint32_t len)
{
    console_write_noflush(buf, len);
    /* Hand a partly filled packet to the host. */
    usb_serial_jtag_ll_txfifo_flush();
}

void console_puts(const char *s)
{
    uint32_t len = 0;

    while (s[len] != '\0') {
        len++;
    }
    console_write(s, len);
}

/*
 * Character sink for the ROM's printf (esp_rom_printf), which the HAL, the
 * Xtensa layer and the fault handlers print through.  Sent to the host one
 * line at a time.
 */
static void console_rom_putc(char c)
{
    if (c == '\n') {
        console_write("\r\n", 2);
    } else {
        console_write_noflush(&c, 1);
    }
}

void console_init(void)
{
    esp_rom_install_channel_putc(1, console_rom_putc);
    esp_rom_install_channel_putc(2, NULL);
}

int console_read_nonblock(char *buf, uint32_t len)
{
    if (!usb_serial_jtag_ll_rxfifo_data_available()) {
        return 0;
    }
    return (int) usb_serial_jtag_ll_read_rxfifo((uint8_t *) buf, len);
}
