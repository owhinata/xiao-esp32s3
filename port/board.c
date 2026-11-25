/*
 * XIAO ESP32S3 board support: LED, CPU clock readback, uptime, console,
 * device ID and restart.
 */
#include <stdint.h>

#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_private/systimer.h"
#include "soc/interrupts.h"
#include "soc/rtc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "hal/efuse_ll.h"
#include "hal/gpio_ll.h"
#include "hal/systimer_hal.h"
#include "hal/usb_serial_jtag_ll.h"
#include "xtensa_api.h"
#include "board.h"

/* ---- LED ----------------------------------------------------------------- */

void board_led_init(void)
{
    gpio_ll_func_sel(&GPIO, BOARD_LED_GPIO, PIN_FUNC_GPIO);
    /* Output driven by the GPIO block itself, not routed from a peripheral. */
    gpio_ll_set_output_signal_matrix_source(&GPIO, BOARD_LED_GPIO, SIG_GPIO_OUT_IDX, false);
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

/* ---- Uptime -------------------------------------------------------------- */

/*
 * SYSTIMER counter 0: 52 bits at 16 MHz, derived from the crystal.  It keeps
 * counting whatever the CPU clock is and while interrupts are masked, which a
 * clock built from the tick count and CCOUNT would have to work around, and
 * it starts from zero at every reset.  ESP-IDF uses the same counter for its
 * esp_timer.
 */
static systimer_hal_context_t uptime_timer;

void board_uptime_init(void)
{
    static systimer_hal_tick_rate_ops_t ops = {
        .ticks_to_us = systimer_ticks_to_us,
        .us_to_ticks = systimer_us_to_ticks,
    };

    systimer_hal_init(&uptime_timer);
    systimer_hal_set_tick_rate_ops(&uptime_timer, &ops);
    systimer_hal_enable_counter(&uptime_timer, SYSTIMER_COUNTER_ESPTIMER);
}

/* The HAL's read repeats itself when an interrupt handler read in between. */
uint64_t board_uptime_us(void)
{
    return systimer_hal_get_time(&uptime_timer, SYSTIMER_COUNTER_ESPTIMER);
}

void board_delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}

/* ---- Device ID and restart ----------------------------------------------- */

/* The factory MAC address in eFuse, in the order it is printed (esptool). */
void board_device_id(uint8_t id[BOARD_DEVICE_ID_SIZE])
{
    uint32_t mac0 = efuse_ll_get_mac0();
    uint32_t mac1 = efuse_ll_get_mac1();

    id[0] = (uint8_t) (mac1 >> 8);
    id[1] = (uint8_t) mac1;
    id[2] = (uint8_t) (mac0 >> 24);
    id[3] = (uint8_t) (mac0 >> 16);
    id[4] = (uint8_t) (mac0 >> 8);
    id[5] = (uint8_t) mac0;
}

void board_restart(void)
{
    esp_rom_software_reset_system();
    for (;;) {
    }
}

/*
 * The ROM looks at this bit, which a software reset leaves alone, before it
 * looks at the strapping pin, and then waits for esptool on USB Serial/JTAG
 * (or UART0) instead of booting from flash.  A reset from esptool ("--after
 * hard-reset") or the RESET button clears it.
 */
void board_restart_to_download(void)
{
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    board_restart();
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

/*
 * The controller does not tell whether a terminal has the port open.  What
 * can be seen is whether the host takes what is written: with the port
 * closed the first packet stays in the FIFO and the next write times out.
 * So the host counts as present until a write has timed out, and again as
 * soon as the FIFO has room.
 */
int console_host_present(void)
{
    if (usb_serial_jtag_ll_txfifo_writable()) {
        console_host_stalled = 0;
    }
    return !console_host_stalled;
}

/*
 * Receive interrupt.  The handler runs in ThreadX interrupt context (the
 * Xtensa layer brackets it with _tx_thread_context_save/restore).  The flag
 * means "a packet arrived", not "the FIFO holds data", so it is cleared
 * before the callback: whoever is woken empties the FIFO, and a packet that
 * arrives meanwhile raises the flag again.
 */
#define CONSOLE_RX_CPU_INTR     12      /* level 1, level-triggered, otherwise unused */

static void (*console_rx_callback)(void);

static void console_rx_isr(void *arg)
{
    (void) arg;
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    console_rx_callback();
}

void console_rx_notify(void (*callback)(void))
{
    console_rx_callback = callback;

    /* Whatever the ROM's own console left enabled has no handler here. */
    usb_serial_jtag_ll_disable_intr_mask(0xFFFFFFFFu);
    usb_serial_jtag_ll_clr_intsts_mask(0xFFFFFFFFu);

    esp_rom_route_intr_matrix(0, ETS_USB_SERIAL_JTAG_INTR_SOURCE, CONSOLE_RX_CPU_INTR);
    xt_set_interrupt_handler(CONSOLE_RX_CPU_INTR, console_rx_isr, NULL);
    usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    xt_ints_on(1u << CONSOLE_RX_CPU_INTR);
}
