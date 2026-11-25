/*
 * XIAO ESP32S3 board support: LED, CPU clock readback, uptime, console,
 * device ID, restart and reset reason (the watchdogs are port/watchdog.c).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tx_api.h"
#include "tx_thread.h"

#include "sdkconfig.h"
#include "esp_cpu.h"
#include "esp_rom_serial_output.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
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
#include "hal/uart_hal.h"
#include "hal/usb_serial_jtag_ll.h"
#include "xtensa_api.h"
#include "xtensa/config/core.h"
#include "xtensa/corebits.h"
#include "xtensa/xtruntime.h"
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
 * esp_timer's clock: SYSTIMER counter 0, 52 bits at 16 MHz, derived from the
 * crystal.  It keeps counting whatever the CPU clock is and while interrupts
 * are masked, which a clock built from the tick count and CCOUNT would have
 * to work around.  esp_timer_early_init(), one of ESP-IDF's start-up hooks
 * that _start runs, resets SYSTIMER and starts the counter from zero.  The
 * HAL's read repeats itself when an interrupt handler read in between.
 */
uint64_t board_uptime_us(void)
{
    return (uint64_t) esp_timer_get_time();
}

void board_delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}

/* ---- Device ID, restart and reset reason --------------------------------- */

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

static void uart_tx_flush(void);

/*
 * The UART's transmit ring is emptied first, so that the last lines (the
 * shell's "Rebooting", the download-mode notice, or the register dump of
 * port/panic.c) are on the line before the reset; uart_tx_flush() says how
 * long that can take.  Polled, so usable with interrupts masked and from the
 * fault handlers.  Then the watchdogs are stopped, so that neither runs on
 * into the bootloader or the ROM's download mode: the RTC watchdog, which
 * the fault path arms, is not reset by the software reset, and the
 * interrupt watchdog is stopped as ESP-IDF's esp_restart_noos() stops it.
 * The software system reset resets the CPU and the digital peripherals and
 * leaves the RAM as it is.
 */
void board_restart(void)
{
    uart_tx_flush();
    board_watchdog_stop();
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

/*
 * The ROM keeps the reason of the last reset; stage 1 of the interrupt
 * watchdog (port/watchdog.c) is timer group 1's system reset.
 */
bool board_reset_by_int_wdt(void)
{
    return esp_rom_get_reset_reason(0) == RESET_REASON_CORE_MWDT1;
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
 * How long to wait for the transmit path of a transport to make progress
 * (room in its TX FIFO, or for the UART room in its ring) before concluding
 * that it does not drain.  Once that happens output to that transport is
 * dropped without waiting until there is room again, so a stalled transport
 * costs one timeout, not one per character.  On USB this is a host that does
 * not read; a UART sends at its baud rate whether anyone listens or not (128
 * bytes, a full FIFO, take 11 ms at 115200 baud), so there it only guards
 * against a transmitter that does not run.
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

/*
 * Transmission.  From a thread, console_write() copies the bytes into a ring
 * and returns; the UART's "TX FIFO below the empty threshold" interrupt moves
 * them from the ring into the 128-byte FIFO.  This is how ESP-IDF's UART
 * driver serves its TX ring buffer (uart_rx_intr_handler_default() and
 * uart_enable_tx_intr() in upper_hal_uart/src/uart.c): the handler disables
 * and clears the interrupt, fills the FIFO, and clears and enables it again
 * while bytes are left; a writer clears and enables it after adding bytes.
 * The status follows the FIFO level: cleared while the FIFO is below the
 * threshold, it is raised again at once.
 *
 * Where the interrupt cannot serve the ring (before the kernel runs, in
 * interrupt handlers, with interrupts masked, in the fault handlers), the
 * writer first moves what is still in the ring into the FIFO and then its own
 * bytes, polling for room.  Either way the bytes reach the line in the order
 * in which they were written.
 *
 * The ring holds 4 KiB: as much as the RAM log (LOG_RAM_SIZE), whose dump is
 * the longest output of the shell; help and ps are well below that.  Such an
 * output is handed over at once and leaves in the background (0.36 s at
 * 115200 baud).  A write that finds the ring full sleeps on a semaphore that
 * the interrupt gives after each FIFO fill (about every 10 ms while the UART
 * transmits), so nothing is cut off while the line runs, whether or not
 * anyone listens.  When no room has appeared for CONSOLE_TX_TIMEOUT_MS the
 * UART counts as stalled, as when its FIFO does not drain, and the rest is
 * dropped.
 *
 * Lock: the ring indices and the UART's interrupt enable register are
 * changed only with interrupts masked up to XCHAL_EXCM_LEVEL (what ThreadX's
 * TX_DISABLE masks: every interrupt with a C handler), raised and never
 * lowered, since the fault handlers run at level 15.  A locked section moves
 * at most UART_TX_CHUNK bytes (a copy into the ring, or as many writes to the
 * FIFO register), so the tick and the receive interrupts are held off for no
 * more than that.  The head advances only once the bytes are in the ring and
 * the tail only once they are in the FIFO, so a fault handler that cuts into
 * any of this finds only bytes that are complete between the two; a fault
 * inside the handler's FIFO fill itself would at worst send part of one
 * chunk twice.
 */
#define UART_TX_RING_SIZE           4096u
#define UART_TX_CHUNK               32u
/*
 * ESP-IDF's default (UART_EMPTY_THRESH_DEFAULT): the interrupt comes with 10
 * bytes, 0.87 ms of line time, still in the FIFO, so the line does not pause
 * for the interrupt latency, and one interrupt moves about 118 bytes.
 */
#define UART_TX_EMPTY_THRESHOLD     10u

_Static_assert((UART_TX_RING_SIZE & (UART_TX_RING_SIZE - 1u)) == 0u,
               "the ring indices wrap with a mask");

static uint8_t uart_tx_ring[UART_TX_RING_SIZE];
static volatile uint32_t uart_tx_head;      /* bytes ever added (wraps) */
static volatile uint32_t uart_tx_tail;      /* bytes ever moved into the FIFO */
static uint8_t uart_tx_irq_installed;       /* console_init() has set up the handler */

/*
 * Threads waiting for room: the interrupt gives the semaphore once for each
 * at the end of every run.  A give for a waiter that has timed out lets a
 * later one look again early, which is harmless.  The semaphore is created by
 * the first thread that writes (state 0 -> 1 -> 2); until then writers poll.
 */
static TX_SEMAPHORE uart_tx_room;
static volatile uint8_t uart_tx_room_state;
static uint32_t uart_tx_waiters;

static inline uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static inline uint32_t uart_tx_lock(void)
{
    return XTOS_SET_MIN_INTLEVEL(XCHAL_EXCM_LEVEL);
}

static inline void uart_tx_unlock(uint32_t ps)
{
    XTOS_RESTORE_INTLEVEL(ps);
}

/* The interrupt enable register is shared by transmit and receive. */
static void uart_intr_enable(uint32_t mask, bool enable)
{
    uint32_t ps = uart_tx_lock();

    if (enable) {
        uart_ll_ena_intr_mask(console_uart.dev, mask);
    } else {
        uart_ll_disable_intr_mask(console_uart.dev, mask);
    }
    uart_tx_unlock(ps);
}

/* Moves up to UART_TX_CHUNK bytes, at most room, from the ring into the FIFO.  Locked. */
static uint32_t uart_tx_ring_to_fifo(uint32_t room)
{
    uint32_t tail = uart_tx_tail;
    uint32_t off = tail & (UART_TX_RING_SIZE - 1u);
    uint32_t n = min_u32(min_u32(uart_tx_head - tail, room),
                         min_u32(UART_TX_CHUNK, UART_TX_RING_SIZE - off));

    uart_ll_write_txfifo(console_uart.dev, &uart_tx_ring[off], n);
    uart_tx_tail = tail + n;
    return n;
}

/*
 * Whether the caller may leave its bytes to the interrupt: a thread, with
 * interrupts enabled, once the kernel runs.  PS.INTLEVEL is nonzero in
 * interrupt handlers (the dispatcher runs them at their level), under an
 * interrupt lockout, in _start and tx_application_define() and in the fault
 * handlers; PS.EXCM is set in exception handlers.
 */
static bool uart_tx_may_queue(void)
{
    uint32_t ps;

    __asm__ volatile ("rsr.ps %0" : "=a" (ps));
    return (ps & (PS_INTLEVEL_MASK | PS_EXCM_MASK)) == 0u
           && TX_THREAD_GET_SYSTEM_STATE() == 0u
           && _tx_thread_current_ptr != TX_NULL
           && uart_tx_irq_installed;
}

/*
 * The semaphore exists, or this thread creates it now.  Not under the lock:
 * creating a ThreadX object may switch threads.
 */
static bool uart_tx_room_ready(void)
{
    uint32_t ps;
    bool claimed;

    if (uart_tx_room_state == 2u) {
        return true;
    }
    ps = uart_tx_lock();
    claimed = uart_tx_room_state == 0u;
    if (claimed) {
        uart_tx_room_state = 1u;
    }
    uart_tx_unlock(ps);
    if (!claimed) {
        return false;
    }
    (void) tx_semaphore_create(&uart_tx_room, "console uart tx", 0);
    uart_tx_room_state = 2u;
    return true;
}

/*
 * Polled, in any context: what is in the ring, then buf.  Each step moves
 * what fits into the FIFO under the lock, so that a handler cutting into this
 * writer, or the interrupt where it can still run (uart_tx_flush() from a
 * thread), takes bytes in order.  The wait for room is not locked.
 */
static void uart_write_polled(const char *buf, uint32_t len)
{
    uart_dev_t *hw = console_uart.dev;

    while (len > 0 || uart_tx_head != uart_tx_tail) {
        uint32_t ps;
        uint32_t room;

        if (!uart_wait_writable()) {
            return;
        }
        ps = uart_tx_lock();
        room = uart_ll_get_txfifo_len(hw);
        if (uart_tx_head != uart_tx_tail) {
            (void) uart_tx_ring_to_fifo(room);
        } else {
            uint32_t n = min_u32(min_u32(room, len), UART_TX_CHUNK);

            uart_ll_write_txfifo(hw, (const uint8_t *) buf, n);
            buf += n;
            len -= n;
        }
        uart_tx_unlock(ps);
    }
}

/* From a thread: into the ring, waiting for room when it is full. */
static void uart_write_queued(const char *buf, uint32_t len)
{
    uart_dev_t *hw = console_uart.dev;
    const ULONG wait = (ULONG) (CONSOLE_TX_TIMEOUT_MS * TX_TIMER_TICKS_PER_SECOND / 1000u);

    while (len > 0) {
        uint32_t ps = uart_tx_lock();
        uint32_t head = uart_tx_head;
        uint32_t off = head & (UART_TX_RING_SIZE - 1u);
        uint32_t n = min_u32(min_u32(len, UART_TX_RING_SIZE - (head - uart_tx_tail)),
                             UART_TX_CHUNK);

        if (n > 0) {
            uint32_t first = min_u32(n, UART_TX_RING_SIZE - off);

            memcpy(&uart_tx_ring[off], buf, first);
            memcpy(&uart_tx_ring[0], buf + first, n - first);
            uart_tx_head = head + n;
            if ((uart_ll_get_intr_ena_status(hw) & UART_INTR_TXFIFO_EMPTY) == 0u) {
                uart_ll_clr_intsts_mask(hw, UART_INTR_TXFIFO_EMPTY);
                uart_ll_ena_intr_mask(hw, UART_INTR_TXFIFO_EMPTY);
            }
            uart_tx_stalled = 0;
            uart_tx_unlock(ps);
            buf += n;
            len -= n;
            continue;
        }
        if (uart_tx_stalled) {
            uart_tx_unlock(ps);
            return;
        }
        uart_tx_waiters++;
        uart_tx_unlock(ps);
        if (tx_semaphore_get(&uart_tx_room, wait) != TX_SUCCESS) {
            uart_tx_stalled = 1;
            return;
        }
    }
}

static void uart_write(const char *buf, uint32_t len)
{
    if (uart_tx_may_queue() && uart_tx_room_ready()) {
        uart_write_queued(buf, len);
    } else {
        uart_write_polled(buf, len);
    }
}

/*
 * TX FIFO below the threshold: refill it from the ring, a chunk per locked
 * section, until the FIFO is full or the ring is empty.  The interrupt stays
 * enabled only while bytes are left, decided under the lock, so that a byte
 * added by a writer is never left without it.  The waiting writers are woken
 * every time, also when this run moved nothing: a polled writer (a handler,
 * say) may have made the room since.  A writer that still finds the ring full
 * waits again; the interrupt is then enabled, since the ring is not empty.
 */
static void uart_tx_isr(void)
{
    uart_dev_t *hw = console_uart.dev;
    uint32_t waiters = 0;
    uint32_t n;

    uart_intr_enable(UART_INTR_TXFIFO_EMPTY, false);
    uart_ll_clr_intsts_mask(hw, UART_INTR_TXFIFO_EMPTY);
    do {
        uint32_t ps = uart_tx_lock();

        n = uart_tx_ring_to_fifo(uart_ll_get_txfifo_len(hw));
        if (n == 0) {
            if (uart_tx_head != uart_tx_tail) {
                uart_ll_clr_intsts_mask(hw, UART_INTR_TXFIFO_EMPTY);
                uart_ll_ena_intr_mask(hw, UART_INTR_TXFIFO_EMPTY);
            }
            waiters = uart_tx_waiters;
            uart_tx_waiters = 0;
        }
        uart_tx_unlock(ps);
    } while (n > 0);

    for (; waiters > 0; waiters--) {
        (void) tx_semaphore_put(&uart_tx_room);
    }
}

/*
 * Before a reset: the ring and the FIFO onto the line, polled, in any
 * context.  At most a full ring and FIFO, 4224 bytes or 0.37 s at 115200
 * baud; CONSOLE_TX_TIMEOUT_MS if the transmitter does not run.
 */
static void uart_tx_flush(void)
{
    uint32_t limit = esp_rom_get_cpu_ticks_per_us() * 1000u * CONSOLE_TX_TIMEOUT_MS;
    uint32_t start;

    uart_write_polled(NULL, 0);
    start = esp_cpu_get_cycle_count();
    while (!uart_tx_stalled && !uart_ll_is_tx_idle(console_uart.dev)) {
        if ((uint32_t) (esp_cpu_get_cycle_count() - start) > limit) {
            return;
        }
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

    /*
     * No interrupts until the first queued write (transmit) and
     * console_rx_notify() (receive); drop what arrived during the boot.
     */
    uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
    uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);
    uart_hal_set_txfifo_empty_thr(&console_uart, UART_TX_EMPTY_THRESHOLD);
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
        uart_intr_enable(UART_INTR_RXFIFO_FULL, true);
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
 * Interrupts.  The handlers run in ThreadX interrupt context (the Xtensa
 * layer brackets them with _tx_thread_context_save/restore).
 *
 * USB, receive: the flag means "a packet arrived", not "the FIFO holds data",
 * so it is cleared before the callback: whoever is woken empties the FIFO,
 * and a packet that arrives meanwhile raises the flag again.
 *
 * UART: one handler for both directions.  Transmit: uart_tx_isr().
 * Receive: "RX FIFO holds at least 1 byte" (the full threshold) stays raised
 * until the FIFO has been read, which the callback only arranges for, so the
 * handler masks it; uart_read() unmasks it.  Only enabled bits show in the
 * status, and receive is enabled only once there is a callback.
 */
static void console_rx_isr(void *arg)
{
    (void) arg;
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    console_rx_callback();
}

static void console_uart_isr(void *arg)
{
    uint32_t status = uart_ll_get_intsts_mask(console_uart.dev);

    (void) arg;
    if ((status & UART_INTR_TXFIFO_EMPTY) != 0u) {
        uart_tx_isr();
    }
    if ((status & UART_INTR_RXFIFO_FULL) != 0u) {
        uart_intr_enable(UART_INTR_RXFIFO_FULL, false);
        uart_ll_clr_intsts_mask(console_uart.dev, UART_INTR_RXFIFO_FULL);
        console_rx_callback();
    }
}

void console_rx_notify(void (*callback)(void))
{
    console_rx_callback = callback;

    /* Whatever the ROM's own console left enabled has no handler here. */
    usb_serial_jtag_ll_disable_intr_mask(0xFFFFFFFFu);
    usb_serial_jtag_ll_clr_intsts_mask(0xFFFFFFFFu);

    esp_rom_route_intr_matrix(0, ETS_USB_SERIAL_JTAG_INTR_SOURCE, BOARD_CONSOLE_RX_CPU_INTR);
    usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    xt_ints_on(1u << BOARD_CONSOLE_RX_CPU_INTR);

    uart_ll_clr_intsts_mask(console_uart.dev, UART_INTR_RXFIFO_FULL);
    uart_intr_enable(UART_INTR_RXFIFO_FULL, true);
}

/*
 * Runs in _start, with interrupts masked until the first thread runs.  The
 * UART's handler is installed here rather than in console_rx_notify() so that
 * transmission is interrupt-driven from the first thread on, shell or not;
 * the UART raises nothing before a queued write or console_rx_notify()
 * enables it.  The USB handler is installed here too; its source is routed
 * and enabled only by console_rx_notify().
 */
void console_init(void)
{
    uart_init();

    xt_set_interrupt_handler(BOARD_CONSOLE_RX_CPU_INTR, console_rx_isr, NULL);

    esp_rom_route_intr_matrix(0, ETS_UART0_INTR_SOURCE, BOARD_CONSOLE_UART_CPU_INTR);
    xt_set_interrupt_handler(BOARD_CONSOLE_UART_CPU_INTR, console_uart_isr, NULL);
    xt_ints_on(1u << BOARD_CONSOLE_UART_CPU_INTR);
    uart_tx_irq_installed = 1;

    esp_rom_install_channel_putc(1, console_rom_putc);
    esp_rom_install_channel_putc(2, NULL);
}
