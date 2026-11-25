/*
 * XIAO ESP32S3 board support: LED, CPU clock readback, uptime, console,
 * device ID and restart.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_cpu.h"
#include "esp_rom_serial_output.h"
#include "esp_rom_sys.h"
#include "esp_private/systimer.h"
#include "soc/interrupts.h"
#include "soc/rtc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/uart_pins.h"
#include "hal/efuse_ll.h"
#include "hal/gpio_ll.h"
#include "hal/systimer_hal.h"
#include "hal/uart_hal.h"
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
 * The clock tree is set up by the second-stage bootloader and _start
 * (BOARD_CPU_FREQ_MHZ).  Read the selection back rather than assume it,
 * since the ThreadX tick is derived from this number.
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
 * and UART0 instead of loading the second-stage bootloader from flash.
 * esptool clears it before the reset of "--after hard-reset", script/flash.py
 * before esptool's "--after watchdog-reset" (which leaves it alone), and the
 * RESET button resets it.
 */
void board_restart_to_download(void)
{
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    board_restart();
}

/* ---- Console ------------------------------------------------------------- */

/*
 * One console on two transports: the USB Serial/JTAG controller (the USB-C
 * connector) and UART0 on D6 (TX, GPIO43) and D7 (RX, GPIO44).  What is
 * written goes to both, what arrives on either is read.  Bytes are passed as
 * they are: callers hand over "\r\n" line ends, except esp_rom_printf(),
 * whose sink (console_rom_putc) converts its "\n".
 */

/*
 * How long to wait for room in a TX FIFO before concluding that it does not
 * drain.  Once that happens output to that transport is dropped without
 * waiting until the FIFO has room again, so a stalled transport costs one
 * timeout, not one per character.  On USB this is a host that does not read;
 * a UART sends at its baud rate whether anyone listens or not (128 bytes, a
 * full FIFO, take 11 ms at 115200 baud), so there it only guards against a
 * transmitter that does not run.
 */
#define CONSOLE_TX_TIMEOUT_MS   50u

/* ---- USB Serial/JTAG ---- */

static uint8_t usb_host_stalled;

static int usb_wait_writable(void)
{
    uint32_t start;
    uint32_t limit;

    if (usb_serial_jtag_ll_txfifo_writable()) {
        usb_host_stalled = 0;
        return 1;
    }
    if (usb_host_stalled) {
        return 0;
    }

    limit = esp_rom_get_cpu_ticks_per_us() * 1000u * CONSOLE_TX_TIMEOUT_MS;
    start = esp_cpu_get_cycle_count();
    while (!usb_serial_jtag_ll_txfifo_writable()) {
        if ((uint32_t) (esp_cpu_get_cycle_count() - start) > limit) {
            usb_host_stalled = 1;
            return 0;
        }
    }
    return 1;
}

static void usb_write_noflush(const char *buf, uint32_t len)
{
    while (len > 0) {
        uint32_t n;

        if (!usb_wait_writable()) {
            return;
        }
        n = usb_serial_jtag_ll_write_txfifo((const uint8_t *) buf, len);
        buf += n;
        len -= n;
    }
}

static void usb_write(const char *buf, uint32_t len)
{
    usb_write_noflush(buf, len);
    /* Hand a partly filled packet to the host. */
    usb_serial_jtag_ll_txfifo_flush();
}

/*
 * The controller does not tell whether a terminal has the port open.  What
 * can be seen is whether the host takes what is written: with the port
 * closed the first packet stays in the FIFO and the next write times out.
 * So the host counts as present until a write has timed out, and again as
 * soon as the FIFO has room.
 */
static int usb_host_present(void)
{
    if (usb_serial_jtag_ll_txfifo_writable()) {
        usb_host_stalled = 0;
    }
    return !usb_host_stalled;
}

/* ---- UART0 ---- */

/* The pins are UART0's own IO MUX pins. */
_Static_assert(CONFIG_ESP_CONSOLE_UART_NUM == 0, "the console UART is UART0");
#define CONSOLE_UART            UART_NUM_0

static uart_hal_context_t console_uart = { .dev = UART_LL_GET_HW(CONSOLE_UART) };
static uint8_t uart_tx_stalled;
static uint8_t uart_rx_seen;

static int uart_wait_writable(void)
{
    uint32_t start;
    uint32_t limit;

    if (uart_ll_get_txfifo_len(console_uart.dev) > 0) {
        uart_tx_stalled = 0;
        return 1;
    }
    if (uart_tx_stalled) {
        return 0;
    }

    limit = esp_rom_get_cpu_ticks_per_us() * 1000u * CONSOLE_TX_TIMEOUT_MS;
    start = esp_cpu_get_cycle_count();
    while (uart_ll_get_txfifo_len(console_uart.dev) == 0) {
        if ((uint32_t) (esp_cpu_get_cycle_count() - start) > limit) {
            uart_tx_stalled = 1;
            return 0;
        }
    }
    return 1;
}

static void uart_write(const char *buf, uint32_t len)
{
    while (len > 0) {
        uint32_t n;

        if (!uart_wait_writable()) {
            return;
        }
        n = uart_ll_get_txfifo_len(console_uart.dev);
        if (n > len) {
            n = len;
        }
        uart_ll_write_txfifo(console_uart.dev, (const uint8_t *) buf, n);
        buf += n;
        len -= n;
    }
}

/*
 * The ROM and the bootloader (CONFIG_ESP_CONSOLE_UART) leave UART0 running
 * at CONFIG_ESP_CONSOLE_UART_BAUDRATE from the crystal, on its IO MUX pins.
 * Set all of it again rather than rely on that, the way ESP-IDF's
 * application start-up and UART driver do for the console UART: wait until
 * the bootloader's last line is out, keep the peripheral running (no reset:
 * that is only done for UARTs that are not the console), then frame format,
 * clock and baud rate.  The crystal clock does not change with the CPU or
 * APB clock.
 */
static void uart_init(void)
{
    uart_dev_t *hw = console_uart.dev;

    esp_rom_output_tx_wait_idle(CONSOLE_UART);

    uart_hal_init(&console_uart, CONSOLE_UART);         /* 8N1, no hardware flow control */
    uart_hal_set_sw_flow_ctrl(&console_uart, NULL, false);
    uart_ll_set_sclk(hw, (soc_module_clk_t) UART_SCLK_XTAL);
    uart_ll_set_baudrate(hw, CONFIG_ESP_CONSOLE_UART_BAUDRATE,
                         (uint32_t) rtc_clk_xtal_freq_get() * 1000000u);

    /*
     * Pins, as gpio_hal_iomux_out() and gpio_hal_iomux_in() set them: the
     * IO MUX function of GPIO43/44 is UART0 itself, so neither signal goes
     * through the GPIO matrix.  RX gets the pull-up (ESP-IDF's bootloader
     * does the same for a console RX pin), so that an unconnected D7 idles
     * high like a connected adapter's TX and receives nothing.
     */
    gpio_ll_func_sel(&GPIO, U0TXD_GPIO_NUM, U0TXD_MUX_FUNC);
    gpio_ll_set_input_signal_from(&GPIO, U0RXD_IN_IDX, false);
    gpio_ll_input_enable(&GPIO, U0RXD_GPIO_NUM);
    gpio_ll_pulldown_dis(&GPIO, U0RXD_GPIO_NUM);
    gpio_ll_pullup_en(&GPIO, U0RXD_GPIO_NUM);
    gpio_ll_func_sel(&GPIO, U0RXD_GPIO_NUM, U0RXD_MUX_FUNC);

    /* No interrupts until console_rx_notify(); drop what arrived during the boot. */
    uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
    uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);
    uart_hal_set_rxfifo_full_thr(&console_uart, 1);
    uart_ll_set_rx_tout(hw, 0);
    uart_ll_rxfifo_rst(hw);
}

/* ---- Both ---- */

void console_write(const char *buf, uint32_t len)
{
    usb_write(buf, len);
    uart_write(buf, len);
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
 * Xtensa layer and the fault handlers print through.  Sent to the USB host
 * one line at a time.
 */
static void console_rom_putc(char c)
{
    if (c == '\n') {
        console_write("\r\n", 2);
    } else {
        usb_write_noflush(&c, 1);
        uart_write(&c, 1);
    }
}

void console_init(void)
{
    uart_init();
    esp_rom_install_channel_putc(1, console_rom_putc);
    esp_rom_install_channel_putc(2, NULL);
}

static void (*console_rx_callback)(void);

/*
 * The UART's receive interrupt is masked from the interrupt until the FIFO
 * has been read (see console_rx_notify()); clearing the status first means
 * that a byte arriving during the read raises it again.
 */
static uint32_t uart_read(char *buf, uint32_t len)
{
    uart_dev_t *hw = console_uart.dev;
    uint32_t n;

    uart_ll_clr_intsts_mask(hw, UART_INTR_RXFIFO_FULL);
    n = uart_ll_get_rxfifo_len(hw);
    if (n > len) {
        n = len;
    }
    uart_ll_read_rxfifo(hw, (uint8_t *) buf, n);
    if (n > 0) {
        uart_rx_seen = 1;
    }
    if (console_rx_callback != NULL) {
        uart_ll_ena_intr_mask(hw, UART_INTR_RXFIFO_FULL);
    }
    return n;
}

int console_read_nonblock(char *buf, uint32_t len)
{
    uint32_t n = 0;

    if (usb_serial_jtag_ll_rxfifo_data_available()) {
        n = (uint32_t) usb_serial_jtag_ll_read_rxfifo((uint8_t *) buf, len);
    }
    if (n < len) {
        n += uart_read(buf + n, len - n);
    }
    return (int) n;
}

/*
 * A UART has no notion of a terminal being there: what it sends goes out
 * whether or not anyone listens.  The only sign of one is that something
 * arrives, and an unconnected RX pin receives nothing (it is pulled up).  So
 * the console counts as connected while the USB host takes what is written,
 * or for good once a byte has come in on the UART.  Until then this is
 * exactly the USB answer.
 */
int console_host_present(void)
{
    return usb_host_present() || uart_rx_seen;
}

/*
 * Receive interrupts.  The handlers run in ThreadX interrupt context (the
 * Xtensa layer brackets them with _tx_thread_context_save/restore).
 *
 * USB: the flag means "a packet arrived", not "the FIFO holds data", so it is
 * cleared before the callback: whoever is woken empties the FIFO, and a
 * packet that arrives meanwhile raises the flag again.
 *
 * UART: "RX FIFO holds at least 1 byte" (the full threshold) stays raised
 * until the FIFO has been read, which the callback only arranges for, so the
 * handler masks it; uart_read() unmasks it.
 */
#define CONSOLE_RX_CPU_INTR         12  /* level 1, level-triggered, otherwise unused */
#define CONSOLE_UART_RX_CPU_INTR    13  /* level 1, level-triggered, otherwise unused */

static void console_rx_isr(void *arg)
{
    (void) arg;
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    console_rx_callback();
}

static void console_uart_rx_isr(void *arg)
{
    (void) arg;
    uart_ll_disable_intr_mask(console_uart.dev, UART_INTR_RXFIFO_FULL);
    uart_ll_clr_intsts_mask(console_uart.dev, UART_INTR_RXFIFO_FULL);
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

    esp_rom_route_intr_matrix(0, ETS_UART0_INTR_SOURCE, CONSOLE_UART_RX_CPU_INTR);
    xt_set_interrupt_handler(CONSOLE_UART_RX_CPU_INTR, console_uart_rx_isr, NULL);
    uart_ll_clr_intsts_mask(console_uart.dev, UART_INTR_RXFIFO_FULL);
    uart_ll_ena_intr_mask(console_uart.dev, UART_INTR_RXFIFO_FULL);
    xt_ints_on(1u << CONSOLE_UART_RX_CPU_INTR);
}
