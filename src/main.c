/*
 * ThreadX on the XIAO ESP32S3: a shell on the console (USB Serial/JTAG and
 * UART0 together), ESP-IDF's esp_timer, whose periodic timer blinks the
 * user LED, and ESP-IDF's Wi-Fi driver in station mode.
 *
 * The shell is rp2040-connect's (shell/common, with this board's part in
 * shell/port).  Its thread "console" is started by the Zephyr compatibility
 * layer it is written on.  esp_timer runs on ThreadX's FreeRTOS
 * compatibility layer: its task "esp_timer" calls the LED callback, woken
 * by the SYSTIMER alarm interrupt, so the LED rate is a direct check of
 * esp_timer, its interrupt and the FreeRTOS layer.  The FreeRTOS task
 * "main", like ESP-IDF's main task, finishes the start-up from a thread and
 * then ends: ESP-IDF's last start-up hooks, then the Wi-Fi driver
 * (port/net/wifi.c).
 */
#include <stdio.h>

#include "tx_api.h"
#include "esp_err.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_log.h"
#include "board.h"
#include "fault_record.h"
#include "log_ram.h"
#include "net/wifi.h"
#include "shell_serial.h"
#include "zephyr_net_wifi.h"

LOG_MODULE_REGISTER(main);

#define PROMPT              "xiao:~$ "

#define BLINK_PERIOD_US     500000u

/* esp_timer callback, in the esp_timer task. */
static void blink(void *arg)
{
    static int on;

    (void) arg;
    on = !on;
    board_led_set(on);
}

/* The LED timer (esp_timer's task and interrupt are set up by then). */
static int blink_start(void)
{
    static const esp_timer_create_args_t blink_timer_args = {
        .callback = blink,
        .name = "blink",
    };
    esp_timer_handle_t timer;

    if (esp_timer_create(&blink_timer_args, &timer) != ESP_OK
        || esp_timer_start_periodic(timer, BLINK_PERIOD_US) != ESP_OK) {
        return -1;
    }
    return 0;
}

/*
 * ESP-IDF's main task (esp_system's main_task()): the start-up hooks that
 * ESP-IDF runs in it, among them the heap's taking over the stack _start
 * ran on, then what app_main() would do.  Its stack is ESP-IDF's default
 * of 3584 bytes plus room for the RF calibration in libphy (esp_wifi_start()),
 * whose depth is not documented.
 */
#define MAIN_TASK_STACK_SIZE    4096u

static void main_task(void *arg)
{
    (void) arg;

    board_sys_pre_app_main();
    (void) net_wifi_start();
    vTaskDelete(NULL);
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

    /*
     * The RAM log ("dmesg") keeps the lines of earlier runs; mark this start
     * in it.  ESP-IDF's log goes there too from now on.
     */
    log_ram_initialize();
    board_esp_log_attach();
    LOG_INF("%s: ThreadX %u.%u.%u, CPU clock %lu Hz", BOARD_NAME,
            THREADX_MAJOR_VERSION, THREADX_MINOR_VERSION, THREADX_PATCH_VERSION,
            (unsigned long) board_cpu_freq_hz());

    /*
     * The record of the fatal error that ended the last run, if any: into
     * the RAM log after the mark of this start, and below the banner.
     */
    fault_boot();
    if (fault_last() != NULL) {
        printf("*** The last run ended with a fatal error (\"fault\" shows it again):\n");
        fault_record_print(fault_last());
    }

    /*
     * The FreeRTOS layer (its byte pool and the thread that reaps deleted
     * tasks), then ESP-IDF's start-up hooks of the stage before its
     * scheduler (esp_timer's task and interrupt), then the LED and "main".
     */
    if (tx_freertos_init() != TX_SUCCESS) {
        printf("The FreeRTOS layer did not start\n");
    }
    board_sys_init_secondary();
    if (blink_start() != 0) {
        printf("The LED timer did not start\n");
    }
    if (xTaskCreate(main_task, "main", MAIN_TASK_STACK_SIZE, NULL, ESP_TASK_MAIN_PRIO, NULL) != pdPASS) {
        printf("The main task did not start\n");
    }

    /*
     * The shell's network layer and its "wifi" command; the driver comes up
     * later, in "main".
     */
    if (zephyr_net_initialize() != 0) {
        printf("The network layer did not start\n");
    }

    /*
     * From here on stdout and stderr of every thread go through the shell's
     * console "serial".  A failure to start the thread leaves a board that
     * blinks but does not answer; say so while the console still works.
     */
    shell_serial_init("serial");
    if (shell_serial_start(PROMPT, NULL) != 0) {
        printf("The shell did not start\n");
    }

    board_sys_pre_scheduler();
}

int main(void)
{
    board_led_init();
    tx_kernel_enter();
    return 0;
}
