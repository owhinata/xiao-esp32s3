/*
 * ESP-IDF's log into the RAM log ("dmesg").
 *
 * ESP_LOGx() of the compiled Espressif sources, and the binary libraries'
 * printf hooks (esp_phy's lib_printf.c turns them into ESP_LOGI()), end in
 * the log component's output function (esp_log_set_vprintf(); log version
 * 1).  A line of the sources arrives as one printf format,
 *
 *   "<letter> (%" PRIu32 ") %s: " message "\n", timestamp, tag, arguments
 *
 * (LOG_FORMAT() of esp_log_format.h, without colours); the binary Wi-Fi
 * libraries write the same text in several calls.  The pieces are joined
 * until the line ends.  Then the letter gives the level, the tag becomes the
 * module of the RAM log's line, and the timestamp is left out: the RAM log
 * stamps every line with its own clock, the same for all of its lines.  So
 * a line reads
 *
 *   [00:00:01.234567] <inf> phy_init: phy_version ...
 *
 * like the lines of the shell's own LOG_INF().  A line without that prefix
 * goes in at Info, with the module "esp".
 *
 * In an interrupt handler the C library may not be used (it would work on
 * the interrupted thread's reentrancy state), and vsnprintf() is the only
 * formatter there is: such a line is stored with its format text as it is,
 * arguments not expanded.  ESP-IDF code logs from interrupt handlers only
 * through ESP_EARLY_LOGx()/ESP_DRAM_LOGx(), which print through the ROM to
 * the console and do not come here.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tx_api.h"
#include "tx_initialize.h"
#include "tx_thread.h"

#include "sdkconfig.h"
#include "esp_log.h"

#include "app_log.h"
#include "log_ram.h"
#include "log_ring.h"

#include "board.h"

/*
 * Wherever this file and app_log.h are both seen, CONFIG_LOG_DEFAULT_LEVEL
 * is the default of the shell's log too (sdkconfig.h): it must stay Info,
 * the shell's default elsewhere.  ESP-IDF's levels have Zephyr's numbers.
 */
_Static_assert(CONFIG_LOG_DEFAULT_LEVEL == LOG_LEVEL_INF,
               "sdkconfig.h's CONFIG_LOG_DEFAULT_LEVEL changes the shell's log default");
_Static_assert(ESP_LOG_ERROR == LOG_LEVEL_ERR && ESP_LOG_WARN == LOG_LEVEL_WRN
               && ESP_LOG_INFO == LOG_LEVEL_INF && ESP_LOG_DEBUG == LOG_LEVEL_DBG,
               "ESP-IDF's and the shell's log levels differ");

/* What follows the level letter in a version-1 line's format. */
static const char line_prefix[] = " (%" PRIu32 ") %s: ";

/* An interrupt handler is running (nesting count; larger values: start-up). */
static bool in_interrupt(void)
{
    ULONG state = TX_THREAD_GET_SYSTEM_STATE();

    return state != 0u && state < TX_INITIALIZE_IN_PROGRESS;
}

static int level_of(char letter)
{
    switch (letter) {
    case 'E':
        return LOG_LEVEL_ERR;
    case 'W':
        return LOG_LEVEL_WRN;
    case 'I':
        return LOG_LEVEL_INF;
    default:                    /* 'D', 'V' */
        return LOG_LEVEL_DBG;
    }
}

/* The length of the version-1 prefix at the start of format, or 0. */
static size_t prefix_length(const char *format)
{
    size_t i;

    if (format[0] != 'E' && format[0] != 'W' && format[0] != 'I'
        && format[0] != 'D' && format[0] != 'V') {
        return 0;
    }
    for (i = 0; line_prefix[i] != '\0'; i++) {
        if (format[1 + i] != line_prefix[i]) {
            return 0;
        }
    }
    return 1 + i;
}

/* Interrupt handlers: the line from the format text, without the C library. */
static void store_unformatted(int level, const char *module, const char *text)
{
    char line[APP_LOG_LINE_MAX];
    size_t room = sizeof(line) - 1;     /* keep one byte for '\n' */
    int n = log_ring_prefix(line, room + 1, board_uptime_us(), level, module, NULL);
    size_t length = n < 0 ? 0 : ((size_t) n < room ? (size_t) n : room);

    while (length < room && *text != '\0' && *text != '\n') {
        line[length++] = *text++;
    }
    line[length++] = '\n';
    log_ram_store(line, length);
}

/*
 * The text of one line, collected from a thread: the binary Wi-Fi libraries
 * write a line in pieces (the prefix, the message, the line end), each with
 * its own call.  owner is the thread whose pieces are in text.
 */
static struct {
    TX_THREAD *owner;
    size_t length;
    char text[APP_LOG_LINE_MAX];
} pending;

/*
 * Stores one complete line.  A line that starts with the version-1 prefix,
 * "<letter> (<number>) <tag>:", gives its level and its tag as the module;
 * any other line goes in at Info with the module "esp".
 */
static void store_line(char *text, size_t length)
{
    int level = LOG_LEVEL_INF;
    const char *module = "esp";
    char *message = text;
    char *p;

    while (length > 0 && (text[length - 1] == '\r' || text[length - 1] == ' ')) {
        length--;
    }
    text[length] = '\0';

    if ((text[0] == 'E' || text[0] == 'W' || text[0] == 'I' || text[0] == 'D' || text[0] == 'V')
        && text[1] == ' ' && text[2] == '(') {
        p = text + 3;
        while (*p >= '0' && *p <= '9') {
            p++;
        }
        if (p > text + 3 && p[0] == ')' && p[1] == ' ') {
            char *tag = p + 2;
            char *colon = strchr(tag, ':');

            if (colon != NULL && colon > tag) {
                level = level_of(text[0]);
                *colon = '\0';
                module = tag;
                message = colon + 1;
                while (*message == ' ') {
                    message++;
                }
            }
        }
    }
    if (*message != '\0') {
        app_log_printf(level, module, NULL, "%s", message);
    }
}

/* Threads: the pieces are joined until the line ends. */
static void collect(const char *text)
{
    TX_THREAD *self = tx_thread_identify();
    char line[APP_LOG_LINE_MAX];
    size_t length;
    TX_INTERRUPT_SAVE_AREA

    for (;;) {
        length = 0;
        TX_DISABLE
        if (pending.owner != self && pending.length > 0) {
            /* Another thread's unfinished line: it goes out as it is. */
            length = pending.length;
            memcpy(line, pending.text, length);
            pending.length = 0;
        } else {
            pending.owner = self;
            while (*text != '\0' && *text != '\n' && pending.length < sizeof(pending.text) - 1u) {
                pending.text[pending.length++] = *text++;
            }
            if (*text == '\n' || pending.length == sizeof(pending.text) - 1u) {
                length = pending.length;
                memcpy(line, pending.text, length);
                pending.length = 0;
                if (*text == '\n') {
                    text++;
                }
            }
        }
        TX_RESTORE
        if (length > 0) {
            store_line(line, length);
        } else if (*text == '\0') {
            return;
        }
    }
}

static int esp_log_ram_vprintf(const char *format, va_list args)
{
    char message[APP_LOG_LINE_MAX];
    int n;

    if (in_interrupt()) {
        size_t skip = prefix_length(format);
        int level = LOG_LEVEL_INF;
        const char *module = "esp";

        if (skip > 0) {
            level = level_of(format[0]);
            (void) va_arg(args, uint32_t);              /* ESP-IDF's timestamp */
            module = va_arg(args, const char *);
            format += skip;
        }
        store_unformatted(level, module, format);
        return 0;
    }

    n = vsnprintf(message, sizeof(message), format, args);
    if (n > 0) {
        collect(message);
    }
    return n;
}

void board_esp_log_attach(void)
{
    (void) esp_log_set_vprintf(esp_log_ram_vprintf);
}
