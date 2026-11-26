/* SPDX-License-Identifier: MIT */
/*
 * "ota": the application's OTA slots (port/ota_slots.h), written as a
 * Zephyr shell command.
 *
 *   ota status    the slot this image runs from, the slot the bootloader
 *                 starts next, and for each slot its place in the flash,
 *                 the otadata entry that selects it (sequence number and
 *                 state) and its image's version and build date, or "no
 *                 image"
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "ota_slots.h"

static const char *slot_name(const struct ota_status *st, int index)
{
    return index >= 0 ? st->slot[index].label : "unknown";
}

static int cmd_ota_status(const struct shell *sh, size_t argc, char **argv)
{
    struct ota_status st;

    (void) argc;
    (void) argv;

    if (ota_slots_status(&st) != 0) {
        shell_error(sh, "No OTA slots in the partition table");
        return -ENODEV;
    }
    shell_print(sh, "running: %s", slot_name(&st, st.running));
    shell_print(sh, "boot: %s", slot_name(&st, st.boot));
    if (!st.otadata) {
        shell_print(sh, "otadata: not readable");
    }
    for (unsigned int i = 0; i < st.count; i++) {
        const struct ota_slot *slot = &st.slot[i];

        shell_print(sh, "%s: 0x%06x, %u KiB", slot->label, (unsigned int) slot->offset,
                    (unsigned int) (slot->size / 1024u));
        if (slot->selected) {
            shell_print(sh, "  otadata: seq %u, %s", (unsigned int) slot->seq, slot->state);
        } else {
            shell_print(sh, "  otadata: no entry");
        }
        if (slot->has_image) {
            shell_print(sh, "  image: %s, built %s %s", slot->version, slot->date, slot->time);
        } else {
            shell_print(sh, "  image: no image");
        }
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ota,
    SHELL_CMD(status, NULL, "Show the OTA slots, otadata and the images", cmd_ota_status),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ota, &sub_ota, "Firmware update slots (OTA)", NULL);
