/*
 * Build-time check only; this file produces no code.
 *
 * The assembly sources reach into TX_THREAD through the numeric offsets in
 * tx_api_asm.h.  Those depend on the kernel version, on tx_user.h options and
 * on the port extensions in tx_port.h, so compare them with what the compiler
 * lays out and fail the build on any drift.
 */
#include <stddef.h>

#include "tx_api.h"

enum {
    c_tx_thread_run_count      = offsetof(TX_THREAD, tx_thread_run_count),
    c_tx_thread_stack_ptr      = offsetof(TX_THREAD, tx_thread_stack_ptr),
    c_tx_thread_stack_end      = offsetof(TX_THREAD, tx_thread_stack_end),
    c_tx_thread_time_slice     = offsetof(TX_THREAD, tx_thread_time_slice),
    c_tx_thread_new_time_slice = offsetof(TX_THREAD, tx_thread_new_time_slice),
    c_tx_thread_solicited      = offsetof(TX_THREAD, tx_thread_solicited),
    c_tx_real_thread_entry     = offsetof(TX_THREAD, tx_real_thread_entry),
    c_tx_thread_clib_ptr       = offsetof(TX_THREAD, tx_thread_clib_ptr),
#if XCHAL_CP_NUM > 0
    c_tx_thread_cp_state       = offsetof(TX_THREAD, tx_thread_cp_state),
    c_tx_thread_cp_state_size  = sizeof(((TX_THREAD *) 0)->tx_thread_cp_state),
#endif
};

/* From here on the member names are the assembly offsets. */
#undef TX_TRUE
#undef TX_FALSE
#undef TX_NULL
#include "tx_api_asm.h"

#define TX_OFFSET_CHECK(field) \
    _Static_assert(c_##field == (field), "tx_api_asm.h: wrong offset of " #field)

TX_OFFSET_CHECK(tx_thread_run_count);
TX_OFFSET_CHECK(tx_thread_stack_ptr);
TX_OFFSET_CHECK(tx_thread_stack_end);
TX_OFFSET_CHECK(tx_thread_time_slice);
TX_OFFSET_CHECK(tx_thread_new_time_slice);
TX_OFFSET_CHECK(tx_thread_solicited);
TX_OFFSET_CHECK(tx_real_thread_entry);
TX_OFFSET_CHECK(tx_thread_clib_ptr);
#if XCHAL_CP_NUM > 0
TX_OFFSET_CHECK(tx_thread_cp_state);
#endif

/* The coprocessor save area the layer indexes must fit the reserved space. */
#if XCHAL_CP_NUM > 0
_Static_assert(c_tx_thread_cp_state_size >= XT_CP_SIZE,
               "tx_thread_cp_state smaller than XT_CP_SIZE");
#endif
