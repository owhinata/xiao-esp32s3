/*
 * Fault reporting.
 *
 * Two entry points, both named by Espressif's code:
 *
 *   xt_unhandled_exception()  default entry of the Xtensa layer's exception
 *                             table: a user-mode exception nobody handles.
 *   panicHandler()            called by _xt_panic (esp_system) for kernel,
 *                             double and debug exceptions and for coprocessor
 *                             use outside a thread.  EXCCAUSE then holds a
 *                             PANIC_RSN_* pseudo cause.
 *
 * Output goes through the ROM printf, which needs no heap and no locks.
 */
#include <stdint.h>

#include "tx_api.h"
#include "tx_thread.h"
#include "xtensa_api.h"
#include "xtensa/xtruntime.h"
#include "esp_private/panic_reason.h"
#include "esp_rom_sys.h"

static const char * const pseudo_reason[PANIC_RSN_MAX + 1] = {
    [PANIC_RSN_NONE]            = "unknown",
    [PANIC_RSN_DEBUGEXCEPTION]  = "debug exception (break instruction?)",
    [PANIC_RSN_DOUBLEEXCEPTION] = "double exception",
    [PANIC_RSN_KERNELEXCEPTION] = "kernel exception",
    [PANIC_RSN_COPROCEXCEPTION] = "coprocessor used outside a thread",
    [PANIC_RSN_INTWDT_CPU0]     = "interrupt watchdog",
    [PANIC_RSN_INTWDT_CPU1]     = "interrupt watchdog",
    [PANIC_RSN_CACHEERR]        = "cache error",
};

static void __attribute__((noreturn)) halt(void)
{
    (void) XTOS_SET_INTLEVEL(15);
    for (;;) {
    }
}

static void dump(const XtExcFrame *f)
{
    const long *a = &f->a0;
    TX_THREAD *t = _tx_thread_current_ptr;
    int i;

    esp_rom_printf("PC=0x%08x PS=0x%08x EXCVADDR=0x%08x SAR=0x%08x",
                   (unsigned) f->pc, (unsigned) f->ps, (unsigned) f->excvaddr, (unsigned) f->sar);
    for (i = 0; i < 16; i++) {
        esp_rom_printf("%sA%d=0x%08x", (i % 4) == 0 ? "\n" : " ", i, (unsigned) a[i]);
    }
    esp_rom_printf("\nthread=%s system_state=0x%08x\nHalted.\n",
                   (t != TX_NULL && t->tx_thread_name != TX_NULL) ? t->tx_thread_name : "(none)",
                   (unsigned) _tx_thread_system_state);
}

void xt_unhandled_exception(XtExcFrame *frame)
{
    (void) XTOS_SET_INTLEVEL(15);
    esp_rom_printf("\n*** Unhandled exception, EXCCAUSE=%d\n", (int) frame->exccause);
    dump(frame);
    halt();
}

void panicHandler(void *frame)
{
    const XtExcFrame *f = frame;
    unsigned cause = (unsigned) f->exccause;

    esp_rom_printf("\n*** Panic: %s\n",
                   cause <= PANIC_RSN_MAX ? pseudo_reason[cause] : pseudo_reason[PANIC_RSN_NONE]);
    dump(f);
    halt();
}

/* Reached through exit(), e.g. main() returning. */
void __attribute__((noreturn)) _exit(int status)
{
    esp_rom_printf("\n*** _exit(%d) -- halted.\n", status);
    halt();
}
