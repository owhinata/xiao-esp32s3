/*
 * ThreadX on the XIAO ESP32S3: a shell on the console (USB Serial/JTAG and
 * UART0 together) and a thread that blinks the user LED.
 *
 * The shell is rp2040-connect's (shell/common, with this board's part in
 * shell/port).  Its thread "console" is started by the Zephyr compatibility
 * layer it is written on; the LED thread is a plain ThreadX thread, paced by
 * tx_thread_sleep(), so the LED rate is a direct check of the tick source.
 */
#include <stdio.h>

#include "tx_api.h"

#include "app_log.h"
#include "board.h"
#include "log_ram.h"
#include "shell_serial.h"

LOG_MODULE_REGISTER(main);

#define PROMPT              "xiao:~$ "

#define BLINK_STACK_SIZE    4096u

static TX_THREAD blink_thread;
static UCHAR     blink_stack[BLINK_STACK_SIZE];

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

/*
 * Runs with interrupts masked, before the first thread: the shell's console
 * and thread need neither an interrupt nor time to pass to be set up.
 */
void tx_application_define(void *first_unused_memory)
{
    (void) first_unused_memory;

    printf("\n%s: ThreadX %u.%u.%u, CPU clock %lu Hz, %lu ticks/s\n",
           BOARD_NAME, THREADX_MAJOR_VERSION, THREADX_MINOR_VERSION, THREADX_PATCH_VERSION,
           (unsigned long) board_cpu_freq_hz(), (unsigned long) TX_TIMER_TICKS_PER_SECOND);

    /* The RAM log ("dmesg") keeps the lines of earlier runs; mark this start in it. */
    log_ram_initialize();
    LOG_INF("%s: ThreadX %u.%u.%u, CPU clock %lu Hz", BOARD_NAME,
            THREADX_MAJOR_VERSION, THREADX_MINOR_VERSION, THREADX_PATCH_VERSION,
            (unsigned long) board_cpu_freq_hz());

    tx_thread_create(&blink_thread, "blink", blink_entry, 0,
                     blink_stack, sizeof(blink_stack),
                     10, 10, TX_NO_TIME_SLICE, TX_AUTO_START);

    /*
     * From here on stdout and stderr of every thread go through the shell's
     * console "serial".  A failure to start the thread leaves a board that
     * blinks but does not answer; say so while the console still works.
     */
    shell_serial_init("serial");
    if (shell_serial_start(PROMPT, NULL) != 0) {
        printf("The shell did not start\n");
    }
}

int main(void)
{
    board_led_init();
    board_uptime_init();
    tx_kernel_enter();
    return 0;
}
