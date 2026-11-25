/*
 * Seeed Studio XIAO ESP32S3 board facts.
 */
#ifndef BOARD_H
#define BOARD_H

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
 * Microseconds since reset; any context, interrupts masked or not.
 * board_uptime_init() once, before the first read.
 */
void     board_uptime_init(void);
uint64_t board_uptime_us(void);

/* Spins for at least us microseconds; any context. */
void     board_delay_us(uint32_t us);

/* Unique per chip: the factory MAC address. */
#define BOARD_DEVICE_ID_SIZE    6
void     board_device_id(uint8_t id[BOARD_DEVICE_ID_SIZE]);

/* Restart, into this firmware or into the ROM's download mode (esptool). */
void     board_restart(void) __attribute__((noreturn));
void     board_restart_to_download(void) __attribute__((noreturn));

/*
 * Console on the built-in USB Serial/JTAG controller (the USB-C connector).
 * Writing and reading are polled and usable with interrupts disabled and
 * from fault handlers.
 */
void     console_init(void);    /* also routes esp_rom_printf() here */
void     console_write(const char *buf, uint32_t len);
void     console_puts(const char *s);
int      console_read_nonblock(char *buf, uint32_t len);

/* Whether the host is taking what is written (a terminal has the port open). */
int      console_host_present(void);

/*
 * Calls callback from the receive interrupt whenever a packet has arrived.
 * Once, from a thread or before the kernel runs.
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
