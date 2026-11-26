/*
 * ThreadX on the XIAO ESP32S3: a shell on the console (USB Serial/JTAG and
 * UART0 together), ESP-IDF's esp_timer, whose periodic timer blinks the
 * user LED, and ESP-IDF's Wi-Fi driver in station mode with lwIP.
 *
 * The shell is rp2040-connect's (shell/common, with this board's part in
 * shell/port).  Its thread "console" is started by the Zephyr compatibility
 * layer it is written on.  esp_timer runs on ThreadX's FreeRTOS
 * compatibility layer: its task "esp_timer" calls the LED callback, woken
 * by the SYSTIMER alarm interrupt, so the LED rate is a direct check of
 * esp_timer, its interrupt and the FreeRTOS layer.  The thread "main",
 * like ESP-IDF's main task, finishes the start-up from a thread and then
 * ends: ESP-IDF's last start-up hooks, the persistent store in flash (the
 * shell's "kv") with the stored Wi-Fi networks ("wifi cred"), then the
 * Wi-Fi driver and lwIP (port/net/wifi.c, port/net/wlanif.c), then the
 * shell on Telnet (port 23), on lwIP's sockets, the OTA service (port
 * 3232) and the network applications: the time synchronization ("ntp"),
 * the iperf3 server of "zperf" and the automatic connection to a stored
 * network.
 */
#include <stdio.h>
#include <stdlib.h>

#include "tx_api.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "app_log.h"
#include "board.h"
#include "esp_compat/pthread_local.h"
#include "fault_record.h"
#include "flash_kv_os.h"
#include "kv_store.h"
#include "log_ram.h"
#include "net/wifi.h"
#include "shell_serial.h"
#include "ota_service.h"
#include "shell_telnet.h"
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
 * The thread "main", ESP-IDF's main task (esp_system's main_task()): the
 * start-up hooks that ESP-IDF runs in it, among them the heap's taking over
 * the stack _start ran on, then what app_main() would do.  Its stack is
 * ESP-IDF's default of 3584 bytes plus room for the RF calibration in libphy
 * (esp_wifi_start()), whose depth is not documented; its priority is that
 * of ESP-IDF's main task (FreeRTOS priority 1), with the shell's thread.
 *
 * The control block and the stack are one block of the heap, which goes
 * back once the thread has ended: a thread cannot delete itself, so its
 * last act is to start a one-shot timer, and the timer's callback, in the
 * esp_timer task, deletes it.
 */
#define MAIN_THREAD_STACK_SIZE  4096u
#define MAIN_THREAD_PRIORITY    30u
#define MAIN_THREAD_REAP_US     10000u

static TX_THREAD *main_thread;      /* the control block, then the stack */
static esp_timer_handle_t main_thread_reaper;

/*
 * esp_timer callback.  The thread has returned from main_thread_entry() by
 * now, or is about to and holds nothing: end it if it has not ended, run
 * the destructors of its thread-specific data (lwIP's semaphore of the
 * thread) and free it.
 */
static void main_thread_reap(void *arg)
{
    (void) arg;
    (void) tx_thread_terminate(main_thread);
    esp_compat_pthread_destructors(main_thread);
    if (tx_thread_delete(main_thread) == TX_SUCCESS) {
        free(main_thread);
    }
    (void) esp_timer_delete(main_thread_reaper);
}

static void main_thread_entry(ULONG arg)
{
    static const esp_timer_create_args_t reaper_args = {
        .callback = main_thread_reap,
        .name = "main_reap",
    };
    int ret;

    (void) arg;

    board_sys_pre_app_main();
    /*
     * The persistent store (Zephyr's settings on NVS in the "settings"
     * partition), mounted from a thread as rp2040-connect's application
     * start does it.  Data that is neither Zephyr's NVS nor erased is not
     * touched: every call of the store fails with -EILSEQ until "kv wipe
     * confirm" (shell/common/compat/zephyr_flash_kv.c logs why).
     */
    ret = kv_store_init();
    if (ret) {
        LOG_ERR("Could not initialize settings: %d", ret);
    }
    /* The stored networks of "wifi cred", from the store. */
    zephyr_net_wifi_credentials_init();
    /*
     * Once lwIP runs: the Telnet shell's listening socket and threads, as
     * rp2040-connect starts it after its network interface (a failure is
     * logged and leaves no Telnet), the OTA service (TCP port 3232,
     * script/ota.py), then the time synchronization, the iperf3 server's
     * thread and the automatic connection to a stored network (the Wi-Fi
     * profile), as rp2040-connect's application start does them last.
     */
    if (net_wifi_start() == 0) {
        (void) shell_telnet_init(PROMPT);
        (void) ota_service_init();
        (void) zephyr_net_app_start();
    }

    /* Without the timer the thread's memory stays allocated. */
    if (esp_timer_create(&reaper_args, &main_thread_reaper) != ESP_OK
        || esp_timer_start_once(main_thread_reaper, MAIN_THREAD_REAP_US) != ESP_OK) {
        LOG_ERR("The main thread's memory is not freed");
    }
}

static int main_thread_start(void)
{
    main_thread = malloc(sizeof(*main_thread) + MAIN_THREAD_STACK_SIZE);
    if (main_thread == NULL) {
        return -1;
    }
    if (tx_thread_create(main_thread, "main", main_thread_entry, 0, main_thread + 1,
                         MAIN_THREAD_STACK_SIZE, MAIN_THREAD_PRIORITY, MAIN_THREAD_PRIORITY,
                         TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
        free(main_thread);
        return -1;
    }
    return 0;
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
     * The store's area, once the flash driver has checked the partition
     * table on the board (port/board_flash.c); no flash access here.  A
     * failure is logged and leaves the store closed.
     */
    (void) flash_kv_os_initialize();

    /*
     * The FreeRTOS layer and ESP-IDF's start-up hooks of the stage before
     * its scheduler (esp_timer's task and interrupt), then the LED and
     * "main".
     */
    board_sys_init_secondary();
    if (blink_start() != 0) {
        printf("The LED timer did not start\n");
    }
    if (main_thread_start() != 0) {
        printf("The main thread did not start\n");
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
