/*
 * Seeed Studio XIAO ESP32S3 board facts.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOARD_NAME              "XIAO ESP32S3"

/* Main crystal. */
#define BOARD_XTAL_FREQ_HZ      40000000u

/* CPU clock the start-up code switches to: 80, 160 or 240 (PLL). */
#define BOARD_CPU_FREQ_MHZ      240

/* Orange user LED on GPIO21, lit when the pin is driven low. */
#define BOARD_LED_GPIO          21
#define BOARD_LED_ACTIVE_LEVEL  0

void     board_led_init(void);
void     board_led_set(int on);

/* CPU clock as currently configured, read back from the clock registers. */
uint32_t board_cpu_freq_hz(void);

/*
 * Microseconds since board_uptime_init(), esp_timer_get_time(); any context,
 * interrupts masked or not.  board_uptime_init() once, before the kernel
 * starts and before the first read: it is esp_timer's early initialization.
 */
void     board_uptime_init(void);
uint64_t board_uptime_us(void);

/* Spins for at least us microseconds; any context. */
void     board_delay_us(uint32_t us);

/* Unique per chip: the factory MAC address. */
#define BOARD_DEVICE_ID_SIZE    6
void     board_device_id(uint8_t id[BOARD_DEVICE_ID_SIZE]);

/*
 * Restart, into this firmware or into the ROM's download mode (esptool),
 * once what is queued for the UART is on the line (at most about 0.4 s).
 * The watchdogs below are stopped just before the reset.
 */
void     board_restart(void) __attribute__((noreturn));
void     board_restart_to_download(void) __attribute__((noreturn));

/*
 * Watchdogs (port/watchdog.c, in RAM).  The interrupt watchdog starts with
 * the first tick and is fed by every tick interrupt, through
 * board_tick_interrupt(), the Xtensa layer's tick handler.  Its first stage
 * ends in panicHandler() (port/panic.c); its second resets the system.
 *
 * board_watchdog_fault(): at the entry of the fault path, interrupts
 * masked.  The interrupt watchdog off, the RTC watchdog armed to reset the
 * system if the fault path does not get to its restart.
 * board_watchdog_stop(): both off (board_restart(), right before the reset).
 */
void     board_tick_interrupt(void);
void     board_watchdog_fault(void);
void     board_watchdog_stop(void);

/*
 * Whether the last reset was the interrupt watchdog's second stage: the
 * chip's reset reason, which every reset sets anew.
 */
bool     board_reset_by_int_wdt(void);

/*
 * Console on two transports at once: the built-in USB Serial/JTAG controller
 * (the USB-C connector) and UART0 on D6 (TX, GPIO43) / D7 (RX, GPIO44) at
 * CONFIG_ESP_CONSOLE_UART_BAUDRATE, 8N1.  Writes go to both, reads take what
 * arrived on either.  Usable in any context, including with interrupts
 * disabled and from fault handlers.  USB is written polled.  The UART is
 * written through a ring that its transmit interrupt empties when the caller
 * is a thread with interrupts enabled (such a write returns once the bytes
 * are in the ring, waiting only while it is full); elsewhere the ring is
 * emptied and the bytes written polled, in order.  Reading is polled.  Bytes
 * are passed unchanged (no line-end conversion).
 */
void     console_init(void);    /* sets up UART0 and its interrupt; also routes esp_rom_printf() here */
void     console_write(const char *buf, uint32_t len);
void     console_puts(const char *s);
int      console_read_nonblock(char *buf, uint32_t len);

/*
 * Whether someone is at the console: the USB host is taking what is written
 * (a terminal has the port open), or a byte has arrived on the UART since
 * the start (a UART cannot tell more).
 */
int      console_host_present(void);

/*
 * Calls callback from the receive interrupts whenever a USB packet or a
 * UART byte has arrived.  Once, from a thread or before the kernel runs.
 */
void     console_rx_notify(void (*callback)(void));

/*
 * Where newlib's stdout and stderr go (port/syscalls.c).  NULL, the initial
 * state: straight to the console.  The sink gets the bytes as written (bare
 * "\n") and is called from threads and before the kernel runs them, never
 * from an interrupt handler.
 */
void     console_stdout_redirect(void (*sink)(const char *buf, size_t len));

#endif
