/* SPDX-License-Identifier: MIT */
/*
 * The record of a fatal error after the restart (fault_record.h): its text,
 * and taking it at the start.  Runs from flash, in threads and in
 * tx_application_define(); the fault handlers only call
 * fault_cause_name() and fault_reason_name() here, for their dump.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "tx_api.h"
#include "tx_initialize.h"

#include "app_log.h"
#include "board.h"
#include "fault_record.h"

LOG_MODULE_REGISTER(fault);

/* This start's copy of the record the last run left. */
static struct fault_record last;
static bool last_valid;

bool fault_record_valid(const struct fault_record *r)
{
    return r->magic == FAULT_RECORD_MAGIC && r->size == sizeof(*r)
           && r->check == fault_record_crc32(r, offsetof(struct fault_record, check));
}

const char *fault_reason_name(uint32_t reason)
{
    switch (reason) {
    case FAULT_REASON_EXCEPTION:
        return "unhandled exception";
    case FAULT_REASON_PANIC:
        return "panic";
    case FAULT_REASON_STACK_OVERFLOW:
        return "stack overflow";
    case FAULT_REASON_EXIT:
        return "exit";
    case FAULT_REASON_INT_WDT:
        return "interrupt watchdog";
    case FAULT_REASON_INT_WDT_RESET:
        return "interrupt watchdog reset";
    default:
        return "unknown";
    }
}

/*
 * EXCCAUSE, in the words of ESP-IDF's panic output (panic_arch_fill_info()
 * in esp_system/port/arch/xtensa/panic_arch.c); NULL for a reserved value.
 */
static const char *const exccause_name[] = {
    "IllegalInstruction", "Syscall", "InstructionFetchError", "LoadStoreError",
    "Level1Interrupt", "Alloca", "IntegerDivideByZero", "PCValue",
    "Privileged", "LoadStoreAlignment", NULL, NULL,
    "InstrPDAddrError", "LoadStorePIFDataError", "InstrPIFAddrError", "LoadStorePIFAddrError",
    "InstTLBMiss", "InstTLBMultiHit", "InstFetchPrivilege", NULL,
    "InstrFetchProhibited", NULL, NULL, NULL,
    "LoadStoreTLBMiss", "LoadStoreTLBMultihit", "LoadStorePrivilege", NULL,
    "LoadProhibited", "StoreProhibited", NULL, NULL,
    "Cp0Dis", "Cp1Dis", "Cp2Dis", "Cp3Dis",
    "Cp4Dis", "Cp5Dis", "Cp6Dis", "Cp7Dis",
};

/* The PANIC_RSN_* values of esp_private/panic_reason.h (0 to 7). */
static const char *const panic_cause_name[] = {
    "unknown",
    "debug exception (break instruction?)",
    "double exception",
    "kernel exception",
    "coprocessor used outside a thread",
    "interrupt watchdog",
    "interrupt watchdog",
    "cache error",
};

const char *fault_cause_name(const struct fault_record *r)
{
    const char *name = NULL;

    if (r->reason == FAULT_REASON_EXCEPTION
        && r->exccause < sizeof(exccause_name) / sizeof(exccause_name[0])) {
        name = exccause_name[r->exccause];
    } else if ((r->reason == FAULT_REASON_PANIC || r->reason == FAULT_REASON_INT_WDT)
               && r->exccause < sizeof(panic_cause_name) / sizeof(panic_cause_name[0])) {
        name = panic_cause_name[r->exccause];
    }
    return name != NULL ? name : "unknown";
}

/* The kinds of line, in their order. */
enum line_kind {
    LINE_REASON,
    LINE_WHERE,
    LINE_CAUSE,
    LINE_PC_PS,
    LINE_A0,
    LINE_A4,
    LINE_A8,
    LINE_A12,
    LINE_PC_SP,
    LINE_NO_RECORD,
};

#define LINE_KINDS_MAX 8

static unsigned int line_kinds(const struct fault_record *r, enum line_kind kinds[LINE_KINDS_MAX])
{
    unsigned int n = 0;

    kinds[n++] = LINE_REASON;
    if (r->reason == FAULT_REASON_INT_WDT_RESET) {
        kinds[n++] = LINE_NO_RECORD;
        return n;
    }
    kinds[n++] = LINE_WHERE;
    if ((r->flags & FAULT_RECORD_FRAME) != 0u) {
        kinds[n++] = LINE_CAUSE;
        kinds[n++] = LINE_PC_PS;
        kinds[n++] = LINE_A0;
        kinds[n++] = LINE_A4;
        kinds[n++] = LINE_A8;
        kinds[n++] = LINE_A12;
    } else {
        kinds[n++] = LINE_PC_SP;
    }
    return n;
}

static int line_where(const struct fault_record *r, char *buf, size_t len)
{
    const char *isr = (r->system_state != 0u && r->system_state < TX_INITIALIZE_IN_PROGRESS)
                      ? ", in an interrupt handler" : "";
    uint64_t s = r->uptime_us / 1000000u;
    uint64_t us = r->uptime_us % 1000000u;

    if (r->system_state >= TX_INITIALIZE_IN_PROGRESS) {
        return snprintf(buf, len, "Before the scheduler started, %" PRIu64 ".%06" PRIu64
                        " s after the start", s, us);
    }
    if (r->task[0] == '\0') {
        return snprintf(buf, len, "With no task running%s, %" PRIu64 ".%06" PRIu64
                        " s after the start", isr, s, us);
    }
    return snprintf(buf, len, "In task %.*s%s, %" PRIu64 ".%06" PRIu64 " s after the start",
                    (int) sizeof(r->task), r->task, isr, s, us);
}

static int line_regs(const struct fault_record *r, unsigned int first, char *buf, size_t len)
{
    return snprintf(buf, len,
                    "a%u 0x%08" PRIx32 " a%u 0x%08" PRIx32 " a%u 0x%08" PRIx32
                    " a%u 0x%08" PRIx32,
                    first, r->a[first], first + 1u, r->a[first + 1u],
                    first + 2u, r->a[first + 2u], first + 3u, r->a[first + 3u]);
}

size_t fault_record_line(const struct fault_record *r, unsigned int index, char *buf, size_t len)
{
    enum line_kind kinds[LINE_KINDS_MAX];
    unsigned int n = line_kinds(r, kinds);
    int ret;

    if (index >= n || len == 0) {
        return 0;
    }
    switch (kinds[index]) {
    case LINE_REASON:
        ret = snprintf(buf, len, "Fatal error: %s%s%.*s", fault_reason_name(r->reason),
                       r->text[0] != '\0' ? ": " : "", (int) sizeof(r->text), r->text);
        break;
    case LINE_WHERE:
        ret = line_where(r, buf, len);
        break;
    case LINE_CAUSE:
        ret = snprintf(buf, len, "exccause %" PRIu32 " (%s) excvaddr 0x%08" PRIx32,
                       r->exccause, fault_cause_name(r), r->excvaddr);
        break;
    case LINE_PC_PS:
        ret = snprintf(buf, len, "pc 0x%08" PRIx32 " ps 0x%08" PRIx32 " sar 0x%08" PRIx32,
                       r->pc, r->ps, r->sar);
        break;
    case LINE_A0:
        ret = line_regs(r, 0, buf, len);
        break;
    case LINE_A4:
        ret = line_regs(r, 4, buf, len);
        break;
    case LINE_A8:
        ret = line_regs(r, 8, buf, len);
        break;
    case LINE_A12:
        ret = line_regs(r, 12, buf, len);
        break;
    case LINE_NO_RECORD:
        ret = snprintf(buf, len,
                       "No record: the interrupt watchdog reset the chip before a fault handler ran");
        break;
    case LINE_PC_SP:
    default:
        ret = snprintf(buf, len, "pc 0x%08" PRIx32 " sp 0x%08" PRIx32, r->pc, r->a[1]);
        break;
    }
    if (ret < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t) ret < len ? (size_t) ret : len - 1;
}

void fault_record_print(const struct fault_record *r)
{
    char line[128];

    for (unsigned int i = 0; fault_record_line(r, i, line, sizeof(line)) > 0; i++) {
        printf("%s\n", line);
    }
}

/*
 * The record into the RAM log as <err> lines of the module "fault" with this
 * start's time, the lines as "fault" prints them.
 */
static void fault_log(void)
{
    char line[128];

    LOG_ERR("The last run ended with a fatal error:");
    for (unsigned int i = 0; fault_record_line(&last, i, line, sizeof(line)) > 0; i++) {
        LOG_ERR("%s", line);
    }
}

void fault_boot(void)
{
    last_valid = fault_record_valid(&fault_retained);
    if (last_valid) {
        last = fault_retained;
    } else if (board_reset_by_int_wdt()) {
        memset(&last, 0, sizeof(last));
        last.reason = FAULT_REASON_INT_WDT_RESET;
        last_valid = true;
    }
    memset(&fault_retained, 0, sizeof(fault_retained));
    if (last_valid) {
        fault_log();
    }
}

const struct fault_record *fault_last(void)
{
    return last_valid ? &last : NULL;
}
