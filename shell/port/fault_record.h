/* SPDX-License-Identifier: MIT */
/*
 * The record of a fatal error that this board keeps across the restart it
 * makes after the error, and its text: this board's counterpart of
 * rp2040-connect's common/compat/include/fault_record.h, with an Xtensa
 * exception frame in place of the Cortex-M0+ one.  The shell's "fault"
 * (shell/common/shell/kernel_shell.c) includes it by this name; shell/port
 * comes before shell/common/compat/include on the include path.
 *
 * The fault handlers (port/panic.c: an exception no handler takes, the
 * Xtensa layer's panic, ThreadX's stack check, _exit()) fill the record in
 * .noinit (fault_retained), which neither the second-stage bootloader nor
 * _start loads or clears, print a register dump and restart the board.  At
 * the next start fault_boot() takes it once; "fault" and the banner show
 * it, and its lines go into the RAM log ("dmesg").  magic, size and check
 * (a CRC-32 of the rest) tell a record from what a power-on leaves in RAM.
 */
#ifndef SHELL_PORT_FAULT_RECORD_H
#define SHELL_PORT_FAULT_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * "FLTX" in memory.  The size is checked as well, so a record of another
 * layout left in RAM by another firmware fails the check.
 */
#define FAULT_RECORD_MAGIC 0x58544c46u

#define FAULT_RECORD_TASK_LEN 32
#define FAULT_RECORD_TEXT_LEN 32

/*
 * Why the firmware stopped.  A new reason takes the next free value;
 * values are not reused, so that a record of an older firmware does not
 * read as another reason ("unknown").
 */
enum fault_reason {
    FAULT_REASON_NONE = 0,
    /* A user-mode exception that no handler takes (xt_unhandled_exception()):
     * exccause is the CPU's EXCCAUSE.
     */
    FAULT_REASON_EXCEPTION = 1,
    /* The Xtensa layer's _xt_panic (panicHandler()): a kernel, double or
     * debug exception (break, __builtin_trap()), or a coprocessor used
     * outside a thread; exccause is a PANIC_RSN_* pseudo cause.
     */
    FAULT_REASON_PANIC = 2,
    /* ThreadX's stack check (TX_ENABLE_STACK_CHECKING); task is the thread
     * whose stack overflowed.
     */
    FAULT_REASON_STACK_OVERFLOW = 3,
    /* _exit(): exit(), abort() (newlib's assert()), main() returning. */
    FAULT_REASON_EXIT = 4,
};

/* The record's flags. */
#define FAULT_RECORD_FRAME (1u << 0) /* exccause .. a[] are the exception frame */

/* Every field before check is covered by check (CRC-32 of the bytes). */
struct fault_record {
    uint32_t magic;
    uint32_t size; /* sizeof(struct fault_record) */
    uint32_t reason;
    uint32_t flags;
    /*
     * With FAULT_RECORD_FRAME, the exception frame the Xtensa layer saved.
     * Otherwise pc is the address the entry was called from, a[1] the stack
     * pointer in the entry, and the others are 0.
     */
    uint32_t exccause;
    uint32_t pc;
    uint32_t ps;
    uint32_t excvaddr;
    uint32_t sar;
    uint32_t a[16];
    /*
     * _tx_thread_system_state at the error: nonzero below
     * TX_INITIALIZE_IN_PROGRESS in an interrupt handler (the nesting
     * count), from there on before the scheduler started.
     */
    uint32_t system_state;
    uint64_t uptime_us;               /* the SYSTIMER uptime at the error */
    char task[FAULT_RECORD_TASK_LEN]; /* the running thread, "" if none */
    char text[FAULT_RECORD_TEXT_LEN]; /* the message, "" if none */
    uint32_t check;
};

/*
 * The record in RAM that survives the restart (port/panic.c).  Written only
 * by the fault handlers, read and cleared only by fault_boot().
 */
extern struct fault_record fault_retained;

/*
 * CRC-32 (IEEE 802.3, reflected, as zlib's crc32()) of len bytes.  Runs
 * from RAM (port/panic.c).
 */
uint32_t fault_record_crc32(const void *data, size_t len);

/* Sets magic, size and check after the other fields are filled (port/panic.c). */
void fault_record_seal(struct fault_record *r);

/* True if magic, size and check are those of a sealed record. */
bool fault_record_valid(const struct fault_record *r);

/* A short name of a reason ("exception", "panic", ...), "unknown" for a
 * value outside enum fault_reason.
 */
const char *fault_reason_name(uint32_t reason);

/*
 * The name of r->exccause: an EXCCAUSE ("LoadProhibited", ...) for
 * FAULT_REASON_EXCEPTION, a PANIC_RSN_* pseudo cause ("double exception",
 * ...) for FAULT_REASON_PANIC; "unknown" for a value without a name.
 */
const char *fault_cause_name(const struct fault_record *r);

/*
 * Line index of the record's text, without a line end, into buf (truncated
 * to len - 1 characters and terminated).  Returns the length of the line,
 * or 0 when index is past the last line.  The lines:
 *
 *   Fatal error: <reason>[: <text>]
 *   In task <task>[, in an interrupt handler], <s>.<us> s after the start
 *   exccause <n> (<name>) excvaddr 0x........                   (frame only)
 *   pc 0x........ ps 0x........ sar 0x........                  (frame only)
 *   a0 0x........ a1 0x........ a2 0x........ a3 0x........     (frame only,
 *   ... up to a15                                                four lines)
 *   pc 0x........ sp 0x........                                 (no frame)
 *
 * The second line says "With no task running" in place of "In task <task>"
 * when task is "", and "Before the scheduler started" when the error came
 * before it.
 */
size_t fault_record_line(const struct fault_record *r, unsigned int index, char *buf, size_t len);

/* Writes the record's lines to stdout, each followed by "\n" ("fault" and
 * the banner after the restart).
 */
void fault_record_print(const struct fault_record *r);

/*
 * At the start, after log_ram_initialize(): takes the record the last run
 * left in fault_retained, if it is valid, keeps a copy for fault_last(),
 * writes its lines into the RAM log, and clears fault_retained so that the
 * next start does not see it again.  Once, before the kernel runs threads.
 */
void fault_boot(void);

/* The fatal error that ended the run before this start, or NULL. */
const struct fault_record *fault_last(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_PORT_FAULT_RECORD_H */
