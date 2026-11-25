/* SPDX-License-Identifier: MIT */
/*
 * The commands only this board and its kernel can answer ("version", "free",
 * "ps"), written as Zephyr shell commands, and the table of all the commands
 * of the shell (shell_engine_cmds[], shell/common/compat/include/shell_engine.h).
 * Their texts and columns follow the commands of the same names in
 * rp2040-connect's builds on the Pico SDK.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "tx_api.h"
#include "tx_thread.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "board.h"
#include "cpu_load.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "ps_interval.h"
#include "shell_engine.h"

/* ---- version ------------------------------------------------------------- */

/*
 * esp-hal-3rdparty carries no version of its own, only that of the ESP-IDF it
 * mirrors.  On ESP-IDF master that is the release being developed.
 */
static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
    (void) argc;
    (void) argv;

    shell_print(sh, "ThreadX kernel %d.%d.%d.%d, esp-hal-3rdparty (ESP-IDF %d.%d.%d), GCC %s",
                THREADX_MAJOR_VERSION, THREADX_MINOR_VERSION, THREADX_PATCH_VERSION,
                THREADX_BUILD_VERSION, ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR,
                ESP_IDF_VERSION_PATCH, __VERSION__);
    return 0;
}

SHELL_CMD_REGISTER(version, NULL, "Show the versions of the kernel, HAL and compiler", cmd_version);

/* ---- free ---------------------------------------------------------------- */

/*
 * malloc(): ESP-IDF's heap (heap_caps; esp_libc's malloc() takes from the
 * heaps of MALLOC_CAP_DEFAULT): the size of those heaps, of that in use and
 * free, and what they can still take.  heap_caps_init() registers every
 * region at the start and heap_caps_enable_nonos_stack_heaps() the start-up
 * stack's once the first thread runs; nothing is added after that, so the
 * last number is 0.  The kernel has no heap of its own.
 */
static int cmd_free(const struct shell *sh, size_t argc, char **argv)
{
    multi_heap_info_t info;

    (void) argc;
    (void) argv;

    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
    shell_print(sh, "malloc: %u bytes from the system, in use %u, free %u, more to take %u",
                (unsigned int) (info.total_allocated_bytes + info.total_free_bytes),
                (unsigned int) info.total_allocated_bytes, (unsigned int) info.total_free_bytes,
                0u);
    return 0;
}

SHELL_CMD_REGISTER(free, NULL, "Show the use of malloc()", cmd_free);

/* ---- ps ------------------------------------------------------------------ */

/*
 * The run times of ThreadX's execution profile kit (tx_user.h), in CPU
 * cycles (CCOUNT), turned into microseconds of the uptime clock (SYSTIMER):
 * both count the crystal, at BOARD_CPU_FREQ_MHZ cycles per microsecond.
 */
#ifndef TX_EXECUTION_PROFILE_ENABLE
#error "ps needs TX_EXECUTION_PROFILE_ENABLE (port/threadx/inc/tx_user.h)"
#endif

/* The kit's (tx_execution_profile.c), declared in no header. */
extern EXECUTION_TIME _tx_execution_idle_time_total;
extern EXECUTION_TIME _tx_execution_isr_time_total;

/*
 * The time thread has run, its current run included when it is the caller
 * (the kit's start is non-zero only while the thread runs).  Interrupts
 * masked by the caller.
 */
static uint64_t ps_thread_cycles(const TX_THREAD *t)
{
    uint64_t total = t->tx_thread_execution_time_total;
    EXECUTION_TIME_SOURCE_TYPE start = t->tx_thread_execution_time_last_start;

    if (start != 0u) {
        total += (EXECUTION_TIME_SOURCE_TYPE) (TX_EXECUTION_TIME_SOURCE - start);
    }
    return total;
}

static uint64_t ps_cycles_to_us(uint64_t cycles)
{
    return cycles / BOARD_CPU_FREQ_MHZ;
}

/* The threads and the rows of the scheduler's wait and of the interrupts. */
#define PS_TASKS_MAX 24

_Static_assert(PS_TASKS_MAX <= PS_INTERVAL_TASKS_MAX, "ps keeps every task");

struct ps_row {
    const TX_THREAD *thread;
    const char *name;
    UINT priority;
    char state;
    uint64_t run_cycles;
    unsigned int permille;
};

/* The ids of the rows that are no thread's. */
static const char ps_idle_id;
static const char ps_irq_id;

/*
 * X the caller, R ready, B waiting with a timeout, S suspended or waiting
 * without one.
 */
static char ps_state(const TX_THREAD *t)
{
    if (t == _tx_thread_current_ptr) {
        return 'X';
    }
    switch (t->tx_thread_state) {
    case TX_READY:
        return 'R';
    case TX_SUSPENDED:
        return 'S';
    default:
        return t->tx_thread_timer.tx_timer_internal_list_head != TX_NULL ? 'B' : 'S';
    }
}

/* The words from the low end that still hold the fill pattern: the least free stack so far. */
static size_t ps_fill_left(const ULONG *start, const ULONG *end)
{
    const ULONG *p = start;

    while (p < end && *p == TX_STACK_FILL) {
        p++;
    }
    return (size_t) ((uintptr_t) p - (uintptr_t) start);
}

static size_t ps_stack_free(const TX_THREAD *t)
{
    return ps_fill_left(t->tx_thread_stack_start,
                        (const ULONG *) ((const uint8_t *) t->tx_thread_stack_end + 1));
}

/*
 * The threads: state, ThreadX priority (0 the most urgent; raised while a
 * mutex's priority inheritance lasts), the least free stack so far and the
 * share of the CPU time since the previous "ps" (since the start for the
 * first), from any shell: the run times of this list less those of the
 * previous one (shell/common/shell/include/ps_interval.h), over the uptime
 * between the two.  Threads that have ended, which ThreadX keeps in its
 * list, are left out.
 *
 * "(idle)" is the time the scheduler waited with no thread ready (ThreadX
 * has no idle thread: the kit's idle time).  "(interrupts)" is the time in
 * the handlers of the interrupts that go through _tx_thread_context_save()
 * (every interrupt of levels 1 to 3), nested ones included, and the least
 * free of the interrupt stack (filled by _tx_initialize_low_level()), on
 * which the scheduler also waits.  This port tells the kit where each
 * interrupt starts and ends, so a handler's time is in that row and not in
 * the thread it interrupted or in "(idle)"; the rows add up to about 100%.
 * What is in no row: the few instructions of a switch between the kit's
 * calls, and for the first list the start before the scheduler ran.
 *
 * "cpu_active" is the time the CPU was not in the scheduler's wait, over
 * the same interval: 100% less "(idle)", rounded down, with the handlers
 * that interrupt the wait counted as load.
 *
 * The list and the run times are read with interrupts masked (the kit
 * changes them at switches and in interrupts), the stacks after: control
 * blocks and stacks are static.
 */
static K_MUTEX_DEFINE(ps_lock);

static int cmd_ps(const struct shell *sh, size_t argc, char **argv)
{
    extern ULONG _xt_interrupt_stack[];
    extern ULONG _xt_interrupt_stack_top[];
    TX_INTERRUPT_SAVE_AREA
    static struct ps_row rows[PS_TASKS_MAX - 2];
    static struct ps_interval interval;
    const TX_THREAD *t;
    uint64_t total;
    uint64_t idle_cycles;
    uint64_t irq_cycles;
    uint64_t elapsed;
    uint64_t idle;
    unsigned int idle_permille;
    unsigned int irq_permille;
    unsigned int count = 0;
    bool full = false;
    ULONG created;
    int active_permille;

    (void) argc;
    (void) argv;

    k_mutex_lock(&ps_lock, K_FOREVER);
    TX_DISABLE
    created = _tx_thread_created_count;
    t = _tx_thread_created_ptr;
    for (ULONG i = 0; i < created; i++, t = t->tx_thread_created_next) {
        struct ps_row *row;

        if (t->tx_thread_state == TX_COMPLETED || t->tx_thread_state == TX_TERMINATED) {
            continue;
        }
        if (count == ARRAY_SIZE(rows)) {
            full = true;
            break;
        }
        row = &rows[count++];
        row->thread = t;
        row->name = t->tx_thread_name;
        row->priority = t->tx_thread_priority;
        row->state = ps_state(t);
        row->run_cycles = ps_thread_cycles(t);
    }
    idle_cycles = _tx_execution_idle_time_total;
    irq_cycles = _tx_execution_isr_time_total;
    total = board_uptime_us();
    TX_RESTORE

    if (full) {
        k_mutex_unlock(&ps_lock);
        shell_error(sh, "More than %d tasks", (int) ARRAY_SIZE(rows));
        return -ENOMEM;
    }

    elapsed = ps_interval_begin(&interval, total);
    for (unsigned int i = 0; i < count; i++) {
        uint64_t run = ps_interval_task(&interval, (uintptr_t) rows[i].thread, 0,
                                        ps_cycles_to_us(rows[i].run_cycles));

        rows[i].permille = ps_interval_permille(run, elapsed);
    }
    idle = ps_interval_task(&interval, (uintptr_t) &ps_idle_id, 0, ps_cycles_to_us(idle_cycles));
    idle_permille = ps_interval_permille(idle, elapsed);
    irq_permille = ps_interval_permille(
        ps_interval_task(&interval, (uintptr_t) &ps_irq_id, 0, ps_cycles_to_us(irq_cycles)), elapsed);
    ps_interval_end(&interval);
    active_permille = cpu_load_ratio(elapsed, idle);

    shell_print(sh, "%-20s %-5s %4s %10s %6s", "NAME", "STATE", "PRIO", "STACK FREE", "CPU");
    for (unsigned int i = 0; i < count; i++) {
        const struct ps_row *row = &rows[i];

        shell_print(sh, "%-20s %-5c %4lu %10lu %4u.%u%%", row->name, row->state,
                    (unsigned long) row->priority, (unsigned long) ps_stack_free(row->thread),
                    row->permille / 10, row->permille % 10);
    }
    /* The scheduler's wait (waiti in _tx_thread_schedule): not a thread. */
    shell_print(sh, "%-20s %-5c %4c %10c %4u.%u%%", "(idle)", '-', '-', '-', idle_permille / 10,
                idle_permille % 10);
    /* The interrupt handlers and their stack: not a thread. */
    shell_print(sh, "%-20s %-5c %4c %10lu %4u.%u%%", "(interrupts)", '-', '-',
                (unsigned long) ps_fill_left(_xt_interrupt_stack, _xt_interrupt_stack_top),
                irq_permille / 10, irq_permille % 10);
    shell_print(sh, "STATE: X running, R ready, B blocked, S suspended");
    shell_print(sh, "cpu_active: %d.%d%% in the %llu.%03llu s since the previous \"ps\"",
                active_permille / 10, active_permille % 10,
                (unsigned long long) (elapsed / 1000000u),
                (unsigned long long) (elapsed / 1000u % 1000u));
    k_mutex_unlock(&ps_lock);
    return 0;
}

SHELL_CMD_REGISTER(ps, NULL, "List the tasks", cmd_ps);

/* ---- The table ----------------------------------------------------------- */

/*
 * The commands of other sources: "help" of the shell engine
 * (shell/common/compat/shell_engine/shell_engine.c), "dmesg"
 * (shell/common/shell/dmesg_shell.c) and the generic commands of
 * shell/common/shell/kernel_shell.c ("date" reads shell/port/zephyr_sys_clock.c,
 * "fault" the record of shell/port/fault_record.h), "wifi"
 * (shell/common/compat/zephyr_wifi_shell.c, on shell/port/zephyr_net_wifi.c),
 * "ifconfig" (shell/common/shell/ifconfig_shell.c, on the interface
 * that shell/port/zephyr_net_wifi.c keeps) and "kv" (shell/common/imu/
 * kv_store.c with "kv wipe" of shell/common/compat/kv_store_wipe.c, on the
 * flash of shell/port/flash_kv_os.c).
 */
SHELL_ENGINE_CMD_DECLARE(bootsel);
SHELL_ENGINE_CMD_DECLARE(date);
SHELL_ENGINE_CMD_DECLARE(devmem);
SHELL_ENGINE_CMD_DECLARE(dmesg);
SHELL_ENGINE_CMD_DECLARE(fault);
SHELL_ENGINE_CMD_DECLARE(help);
SHELL_ENGINE_CMD_DECLARE(id);
SHELL_ENGINE_CMD_DECLARE(ifconfig);
SHELL_ENGINE_CMD_DECLARE(kv);
SHELL_ENGINE_CMD_DECLARE(reboot);
SHELL_ENGINE_CMD_DECLARE(sleep);
SHELL_ENGINE_CMD_DECLARE(uptime);
SHELL_ENGINE_CMD_DECLARE(wifi);

/* Sorted by name, as Zephyr's "help" lists them. */
const struct shell_engine_cmd shell_engine_cmds[] = {
    SHELL_ENGINE_CMD(bootsel),
    SHELL_ENGINE_CMD(date),
    SHELL_ENGINE_CMD(devmem),
    SHELL_ENGINE_CMD(dmesg),
    SHELL_ENGINE_CMD(fault),
    SHELL_ENGINE_CMD(free),
    SHELL_ENGINE_CMD(help),
    SHELL_ENGINE_CMD(id),
    SHELL_ENGINE_CMD(ifconfig),
    SHELL_ENGINE_CMD(kv),
    SHELL_ENGINE_CMD(ps),
    SHELL_ENGINE_CMD(reboot),
    SHELL_ENGINE_CMD(sleep),
    SHELL_ENGINE_CMD(uptime),
    SHELL_ENGINE_CMD(version),
    SHELL_ENGINE_CMD(wifi),
};

const size_t shell_engine_cmd_count = sizeof(shell_engine_cmds) / sizeof(shell_engine_cmds[0]);
