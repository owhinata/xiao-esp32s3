/*
 * ThreadX bring-up demo for the XIAO ESP32S3.
 *
 * Two threads: one blinks the user LED, one prints a heartbeat line on the
 * USB Serial/JTAG console.  Each paces itself with tx_thread_sleep(), so the
 * LED rate and the tick counter are a direct check of the tick source.
 */
#include <stdio.h>

#include "tx_api.h"
#include "esp_cpu.h"

#include "board.h"


#define DEMO_STACK_SIZE     4096u

static TX_THREAD blink_thread;
static TX_THREAD hello_thread;
static UCHAR     blink_stack[DEMO_STACK_SIZE];
static UCHAR     hello_stack[DEMO_STACK_SIZE];

static void blink_entry(ULONG arg)
{
    int on = 0;

    (void) arg;
    for (;;) {
        on = !on;
        board_led_set(on);
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND / 2);
    }
}

static void hello_entry(ULONG arg)
{
    ULONG n = 0;

    (void) arg;
    for (;;) {
        printf("hello %lu  tick=%lu  ccount=%lu\n", n++, tx_time_get(),
               (unsigned long) esp_cpu_get_cycle_count());
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    }
}

void tx_application_define(void *first_unused_memory)
{
    (void) first_unused_memory;

    printf("\n%s: ThreadX %u.%u.%u, CPU clock %lu Hz, %lu ticks/s\n",
           BOARD_NAME, THREADX_MAJOR_VERSION, THREADX_MINOR_VERSION, THREADX_PATCH_VERSION,
           (unsigned long) board_cpu_freq_hz(), (unsigned long) TX_TIMER_TICKS_PER_SECOND);

    tx_thread_create(&blink_thread, "blink", blink_entry, 0,
                     blink_stack, sizeof(blink_stack),
                     10, 10, TX_NO_TIME_SLICE, TX_AUTO_START);
    tx_thread_create(&hello_thread, "hello", hello_entry, 0,
                     hello_stack, sizeof(hello_stack),
                     11, 11, TX_NO_TIME_SLICE, TX_AUTO_START);
}

int main(void)
{
    board_led_init();
    tx_kernel_enter();
    return 0;
}
