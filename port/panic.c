/*
 * Fatal errors: the register dump, the record kept across the restart
 * (shell/port/fault_record.h) and the restart.
 *
 * Four entry points, all named by code elsewhere:
 *
 *   xt_unhandled_exception()  default entry of the Xtensa layer's exception
 *                             table: a user-mode exception nobody handles.
 *   panicHandler()            called by _xt_panic (esp_system) for kernel,
 *                             double and debug exceptions and for coprocessor
 *                             use outside a thread.  EXCCAUSE then holds a
 *                             PANIC_RSN_* pseudo cause.
 *   _tx_xtensa_stack_error_handler()
 *                             ThreadX's stack check (TX_ENABLE_STACK_CHECKING),
 *                             registered by tx_port.h.
 *   _exit()                   exit(), abort(), main() returning.
 *
 * Each masks every interrupt, fills fault_retained in .noinit, prints the
 * dump through the ROM printf and restarts the board with board_restart(),
 * which first lets the UART send what is queued.  Nothing here allocates,
 * locks, uses newlib's stdio or calls a ThreadX service.  fault_boot()
 * takes the record at the next start.
 *
 * This file and its read-only data run from RAM (see the linker script), so
 * the record is filled and sealed without reading flash: the exception
 * frame, the kernel's state and the CRC.  What may read flash is done after
 * that first seal, and the record is sealed again: the uptime
 * (board_uptime_us() and the SYSTIMER HAL are in flash), the thread's name
 * (a string literal is in flash), then the dump, whose console code and
 * name tables are in flash.  A fault on the way, wherever it comes from,
 * enters here again and only resets the board: the record keeps what was
 * last sealed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tx_api.h"
#include "tx_initialize.h"
#include "tx_thread.h"
#include "xtensa_api.h"
#include "xtensa/xtruntime.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"

#include "board.h"
#include "fault_record.h"

/* Left alone by the bootloader and _start (ldscript/xiao_esp32s3.ld). */
struct fault_record fault_retained __attribute__((section(".noinit.fault_record")));

/* Set by the first entry; .bss, so clear at every start. */
static volatile bool fault_entered;

extern char _flash_rodata_start, _flash_rodata_end;

uint32_t fault_record_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;

    while (len-- > 0) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void fault_record_seal(struct fault_record *r)
{
    r->magic = FAULT_RECORD_MAGIC;
    r->size = sizeof(*r);
    r->task[sizeof(r->task) - 1] = '\0';
    r->text[sizeof(r->text) - 1] = '\0';
    r->check = fault_record_crc32(r, offsetof(struct fault_record, check));
}

/* True if [address, address + size) lies inside [start, end); no sum can wrap. */
static bool in_range(uintptr_t address, size_t size, uintptr_t start, uintptr_t end)
{
    return size <= end - start && address >= start && address - start <= (end - start) - size;
}

/* Where a thread's name may be: internal data RAM, or the read-only data in flash. */
static bool name_readable(uintptr_t address)
{
    return in_range(address, 1, SOC_DRAM_LOW, SOC_DRAM_HIGH)
           || in_range(address, 1, (uintptr_t) &_flash_rodata_start, (uintptr_t) &_flash_rodata_end);
}

/*
 * The name of thread into r->task, when its control block is in data RAM
 * and as far as the name lies in memory a name may be in: a fault may come
 * from a corrupted kernel.  "?" when the control block is there but the
 * name is not.  Nothing when there is no thread.
 */
static void copy_task_name(struct fault_record *r, const TX_THREAD *thread)
{
    const char *name;
    size_t i;

    if (thread == TX_NULL || ((uintptr_t) thread & 3u) != 0u
        || !in_range((uintptr_t) thread, sizeof(*thread), SOC_DRAM_LOW, SOC_DRAM_HIGH)) {
        return;
    }
    name = thread->tx_thread_name;
    for (i = 0; i < sizeof(r->task) - 1u && name_readable((uintptr_t) (name + i)) && name[i] != '\0';
         i++) {
        r->task[i] = name[i];
    }
    if (i == 0) {
        r->task[i++] = '?';
    }
    r->task[i] = '\0';
}

/*
 * Masks every interrupt and starts a cleared record with reason and the
 * kernel's state.  A second entry, a fault inside this path, resets at once
 * (a ROM function).
 */
static struct fault_record *fault_begin(uint32_t reason)
{
    struct fault_record *r = &fault_retained;

    (void) XTOS_SET_INTLEVEL(15);
    if (fault_entered) {
        esp_rom_software_reset_system();
        for (;;) {
        }
    }
    fault_entered = true;

    memset(r, 0, sizeof(*r));
    r->reason = reason;
    r->system_state = (uint32_t) _tx_thread_system_state;
    return r;
}

static void set_frame(struct fault_record *r, const XtExcFrame *f)
{
    const long *a = &f->a0;

    r->flags |= FAULT_RECORD_FRAME;
    r->exccause = (uint32_t) f->exccause;
    r->pc = (uint32_t) f->pc;
    r->ps = (uint32_t) f->ps;
    r->excvaddr = (uint32_t) f->excvaddr;
    r->sar = (uint32_t) f->sar;
    for (int i = 0; i < 16; i++) {
        r->a[i] = (uint32_t) a[i];
    }
}

/* No frame: where the entry was called from, and the stack pointer. */
static void set_caller(struct fault_record *r, uint32_t pc)
{
    uint32_t sp;

    __asm__ volatile ("mov %0, a1" : "=a" (sp));
    r->pc = pc;
    r->a[1] = sp;
}

/* text = "status <n>": the decimal digits by hand (no stdio here). */
static void set_status_text(struct fault_record *r, int status)
{
    static const char prefix[] = "status ";
    char digits[12];
    uint32_t v = status < 0 ? 0u - (uint32_t) status : (uint32_t) status;
    size_t n = 0;
    size_t i = sizeof(prefix) - 1u;

    memcpy(r->text, prefix, i);
    do {
        digits[n++] = (char) ('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    if (status < 0) {
        r->text[i++] = '-';
    }
    while (n > 0) {
        r->text[i++] = digits[--n];
    }
    r->text[i] = '\0';
}

/*
 * The parts that may read flash, each followed by a seal: the uptime, then
 * the name of thread (the running one, or the one the entry names).
 */
static void fault_seal(struct fault_record *r, const TX_THREAD *thread)
{
    fault_record_seal(r);
    r->uptime_us = board_uptime_us();
    fault_record_seal(r);
    copy_task_name(r, thread);
    fault_record_seal(r);
}

/* The registers and the kernel's state after the entry's headline, then the restart. */
static void __attribute__((noreturn)) fault_dump_and_restart(const struct fault_record *r)
{
    if ((r->flags & FAULT_RECORD_FRAME) != 0u) {
        esp_rom_printf("PC=0x%08x PS=0x%08x EXCVADDR=0x%08x SAR=0x%08x",
                       (unsigned) r->pc, (unsigned) r->ps, (unsigned) r->excvaddr,
                       (unsigned) r->sar);
        for (int i = 0; i < 16; i++) {
            esp_rom_printf("%sA%d=0x%08x", (i % 4) == 0 ? "\n" : " ", i, (unsigned) r->a[i]);
        }
    } else {
        esp_rom_printf("PC=0x%08x SP=0x%08x", (unsigned) r->pc, (unsigned) r->a[1]);
    }
    esp_rom_printf("\nthread=%s system_state=0x%08x\nRestarting.\n",
                   r->task[0] != '\0' ? r->task : "(none)", (unsigned) r->system_state);
    board_restart();
}

void xt_unhandled_exception(XtExcFrame *frame)
{
    struct fault_record *r = fault_begin(FAULT_REASON_EXCEPTION);

    set_frame(r, frame);
    fault_seal(r, _tx_thread_current_ptr);
    esp_rom_printf("\n*** Unhandled exception, EXCCAUSE=%d (%s)\n", (int) r->exccause,
                   fault_cause_name(r));
    fault_dump_and_restart(r);
}

void panicHandler(void *frame)
{
    struct fault_record *r = fault_begin(FAULT_REASON_PANIC);

    set_frame(r, frame);
    fault_seal(r, _tx_thread_current_ptr);
    esp_rom_printf("\n*** Panic: %s\n", fault_cause_name(r));
    fault_dump_and_restart(r);
}

#ifdef TX_ENABLE_STACK_CHECKING
/*
 * ThreadX calls this when the fill pattern at either end of thread's stack
 * is gone, or its saved stack pointer is outside the stack, as it suspends
 * or resumes thread; from the thread itself or from an interrupt handler.
 * The record names thread, which is not always the running one.
 */
VOID _tx_xtensa_stack_error_handler(TX_THREAD *thread)
{
    struct fault_record *r = fault_begin(FAULT_REASON_STACK_OVERFLOW);

    set_caller(r, (uint32_t) __builtin_return_address(0));
    fault_seal(r, thread);
    esp_rom_printf("\n*** Stack overflow in thread \"%s\"\n", r->task);
    fault_dump_and_restart(r);
}
#endif

void __attribute__((noreturn)) _exit(int status)
{
    struct fault_record *r = fault_begin(FAULT_REASON_EXIT);

    set_caller(r, (uint32_t) __builtin_return_address(0));
    set_status_text(r, status);
    fault_seal(r, _tx_thread_current_ptr);
    esp_rom_printf("\n*** _exit(%d)\n", status);
    fault_dump_and_restart(r);
}
