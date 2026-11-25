/*
 * Seeed Studio XIAO ESP32S3 board facts.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#define BOARD_NAME              "XIAO ESP32S3"

/* Main crystal. */
#define BOARD_XTAL_FREQ_HZ      40000000u

/* Orange user LED on GPIO21, lit when the pin is driven low. */
#define BOARD_LED_GPIO          21
#define BOARD_LED_ACTIVE_LEVEL  0

void     board_led_init(void);
void     board_led_set(int on);

/* CPU clock as currently configured, read back from the clock registers. */
uint32_t board_cpu_freq_hz(void);

/*
 * Console on the built-in USB Serial/JTAG controller (the USB-C connector).
 * Polled; usable with interrupts disabled and from fault handlers.
 */
void     console_init(void);    /* also routes esp_rom_printf() here */
void     console_write(const char *buf, uint32_t len);
void     console_puts(const char *s);
int      console_read_nonblock(char *buf, uint32_t len);

#endif
