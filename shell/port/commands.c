/* SPDX-License-Identifier: MIT */
/*
 * The commands only this board and its kernel can answer ("version", "free",
 * "ps"), written as Zephyr shell commands, and the table of all the commands
 * of the shell (shell_engine_cmds[], shell/common/compat/include/shell_engine.h).
 * Their texts and columns follow the commands of the same names in
 * rp2040-connect's builds on the Pico SDK.
 */
#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "tx_api.h"
#include "tx_thread.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "esp_idf_version.h"
#include "shell_engine.h"

/* ---- version ------------------------------------------------------------- */

/* esp-hal-components carries no version of its own, only that of the ESP-IDF it was cut from. */
static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
    (void) argc;
    (void) argv;

    shell_print(sh, "ThreadX kernel %d.%d.%d.%d, esp-hal-components (ESP-IDF %d.%d.%d), GCC %s",
                THREADX_MAJOR_VERSION, THREADX_MINOR_VERSION, THREADX_PATCH_VERSION,
                THREADX_BUILD_VERSION, ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR,
                ESP_IDF_VERSION_PATCH, __VERSION__);
    return 0;
}

SHELL_CMD_REGISTER(version, NULL, "Show the versions of the kernel, HAL and compiler", cmd_version);

/* ---- free ---------------------------------------------------------------- */

/*
 * newlib's malloc(): what it has taken from _sbrk_r() (the heap region of
 * port/threadx/src/tx_clib_newlib.c), of that in use and free, and what it
 * can still take.  The kernel has no heap of its own.
 */
static int cmd_free(const struct shell *sh, size_t argc, char **argv)
{
    struct mallinfo m = mallinfo();
    /*
     * malloc() is the only caller of _sbrk_r(), so what it holds is what has
     * been taken from the region.  (sbrk() itself must not be called: the
     * name resolves to the ROM's, which goes through a ROM syscall table that
     * nothing here sets up.)
     */
    size_t region = (size_t) (_tx_clib_heap_end - _tx_clib_heap_start);

    (void) argc;
    (void) argv;

    shell_print(sh, "malloc: %u bytes from the system, in use %u, free %u, more to take %u",
                (unsigned int) m.arena, (unsigned int) m.uordblks, (unsigned int) m.fordblks,
                (unsigned int) (region - (size_t) m.arena));
    return 0;
}

SHELL_CMD_REGISTER(free, NULL, "Show the use of malloc()", cmd_free);

/* ---- ps ------------------------------------------------------------------ */

#define PS_THREADS_MAX 24

struct ps_row {
    const TX_THREAD *thread;
    const char *name;
    UINT priority;
    char state;
};

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
 * mutex's priority inheritance lasts) and the least free stack so far.
 * Threads that have ended, which ThreadX keeps in its list, are left out.
 * The last row is the interrupt stack (filled by _tx_initialize_low_level()),
 * on which the scheduler also waits when no thread is ready.
 *
 * rp2040-connect's "ps" has a CPU column and a "cpu_active" line as well.
 * They need the run time of each thread, which this port does not measure,
 * so they are absent here rather than filled with a guess.
 *
 * The list is read with interrupts masked, the stacks after: control blocks
 * and stacks are static.
 */
static K_MUTEX_DEFINE(ps_lock);

static int cmd_ps(const struct shell *sh, size_t argc, char **argv)
{
    extern ULONG _xt_interrupt_stack[];
    extern ULONG _xt_interrupt_stack_top[];
    TX_INTERRUPT_SAVE_AREA
    static struct ps_row rows[PS_THREADS_MAX];
    const TX_THREAD *t;
    unsigned int count = 0;
    bool full = false;
    ULONG created;

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
    }
    TX_RESTORE

    if (full) {
        k_mutex_unlock(&ps_lock);
        shell_error(sh, "More than %d tasks", (int) ARRAY_SIZE(rows));
        return -ENOMEM;
    }

    shell_print(sh, "%-20s %-5s %4s %10s", "NAME", "STATE", "PRIO", "STACK FREE");
    for (unsigned int i = 0; i < count; i++) {
        const struct ps_row *row = &rows[i];

        shell_print(sh, "%-20s %-5c %4lu %10lu", row->name, row->state,
                    (unsigned long) row->priority, (unsigned long) ps_stack_free(row->thread));
    }
    shell_print(sh, "%-20s %-5c %4c %10lu", "(interrupts)", '-', '-',
                (unsigned long) ps_fill_left(_xt_interrupt_stack, _xt_interrupt_stack_top));
    shell_print(sh, "STATE: X running, R ready, B blocked, S suspended");
    k_mutex_unlock(&ps_lock);
    return 0;
}

SHELL_CMD_REGISTER(ps, NULL, "List the tasks", cmd_ps);

/* ---- The table ----------------------------------------------------------- */

/*
 * The commands of other sources: "help" of the shell engine
 * (shell/common/compat/shell_engine/shell_engine.c), "dmesg"
 * (shell/common/shell/dmesg_shell.c) and the generic commands of
 * shell/common/shell/kernel_shell.c.  That file also registers "date" and
 * "fault", which are not listed and so are dropped at link time: nothing
 * sets a wall clock on this board, and the fault record is laid out for a
 * Cortex-M exception frame.
 */
SHELL_ENGINE_CMD_DECLARE(bootsel);
SHELL_ENGINE_CMD_DECLARE(devmem);
SHELL_ENGINE_CMD_DECLARE(dmesg);
SHELL_ENGINE_CMD_DECLARE(help);
SHELL_ENGINE_CMD_DECLARE(id);
SHELL_ENGINE_CMD_DECLARE(reboot);
SHELL_ENGINE_CMD_DECLARE(sleep);
SHELL_ENGINE_CMD_DECLARE(uptime);

/* Sorted by name, as Zephyr's "help" lists them. */
const struct shell_engine_cmd shell_engine_cmds[] = {
    SHELL_ENGINE_CMD(bootsel),
    SHELL_ENGINE_CMD(devmem),
    SHELL_ENGINE_CMD(dmesg),
    SHELL_ENGINE_CMD(free),
    SHELL_ENGINE_CMD(help),
    SHELL_ENGINE_CMD(id),
    SHELL_ENGINE_CMD(ps),
    SHELL_ENGINE_CMD(reboot),
    SHELL_ENGINE_CMD(sleep),
    SHELL_ENGINE_CMD(uptime),
    SHELL_ENGINE_CMD(version),
};

const size_t shell_engine_cmd_count = sizeof(shell_engine_cmds) / sizeof(shell_engine_cmds[0]);
