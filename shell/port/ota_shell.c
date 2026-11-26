/* SPDX-License-Identifier: MIT */
/*
 * "ota": the application's OTA slots (port/ota_slots.h) and the OTA
 * service (shell/port/ota_service.h), written as a Zephyr shell command.
 *
 *   ota status    the slot this image runs from with the state of its
 *                 otadata entry, the slot the bootloader starts next, the
 *                 service's port and the update in flight, and for each
 *                 slot its place in the flash, the otadata entry that
 *                 selects it (sequence number and state) and its image's
 *                 version, build date and ELF SHA-256 (its first 16 hex
 *                 digits), or "no image"
 *   ota confirm   marks the running image VALID when it is pending
 *                 verification (the first start after an update), as
 *                 script/ota.py does through the service
 *   ota rollback  marks the running image INVALID and restarts into the
 *                 other slot, when that slot holds an image the bootloader
 *                 would boot
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "ota_service.h"
#include "ota_slots.h"

/* The output's time to leave before the restart (the kernel shell's "reboot"). */
#define RESTART_DELAY_MS 100

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
    shell_print(sh, "running: %s, %s", slot_name(&st, st.running), st.running_state);
    shell_print(sh, "boot: %s", slot_name(&st, st.boot));
    if (!st.otadata) {
        shell_print(sh, "otadata: not readable");
    }
    if (ota_service_listening()) {
        shell_print(sh, "service: TCP port %u", OTA_SERVICE_PORT);
    } else {
        shell_print(sh, "service: not listening");
    }
    if (st.updating) {
        shell_print(sh, "update: %s, %u of %u bytes (%u %%)", st.update_slot,
                    (unsigned int) st.update_written, (unsigned int) st.update_size,
                    (unsigned int) ((uint64_t) st.update_written * 100u / st.update_size));
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
            shell_print(sh, "  image: %s, built %s %s, ELF SHA-256 %s", slot->version, slot->date,
                        slot->time, slot->elf_sha256);
        } else {
            shell_print(sh, "  image: no image");
        }
    }
    return 0;
}

static int cmd_ota_confirm(const struct shell *sh, size_t argc, char **argv)
{
    struct ota_confirm c;
    int ret;

    (void) argc;
    (void) argv;

    ret = ota_confirm(NULL, &c);
    if (ret == -ENODEV) {
        shell_error(sh, "Not running from an OTA slot");
        return ret;
    }
    if (ret < 0) {
        shell_error(sh, "%s: otadata was not written", c.label);
        return ret;
    }
    if (c.changed) {
        shell_print(sh, "%s: confirmed, %s", c.label, c.state);
    } else {
        shell_print(sh, "%s: %s, nothing to confirm", c.label, c.state);
    }
    return 0;
}

static int cmd_ota_rollback(const struct shell *sh, size_t argc, char **argv)
{
    int ret;

    (void) argc;
    (void) argv;

    if (!ota_rollback_possible()) {
        shell_error(sh, "The other slot holds no image to go back to");
        return -ENOENT;
    }
    shell_print(sh, "Marking this image invalid and restarting into the other slot");
    k_msleep(RESTART_DELAY_MS);
    ret = ota_rollback();
    if (ret == -EBUSY) {
        shell_error(sh, "An update is in flight");
    } else if (ret == -ENOENT) {
        shell_error(sh, "The other slot holds no image to go back to");
    } else {
        shell_error(sh, "otadata was not written");
    }
    return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ota,
    SHELL_CMD(status, NULL, "Show the OTA slots, otadata, the images and the update service", cmd_ota_status),
    SHELL_CMD(confirm, NULL, "Confirm the running image after an update", cmd_ota_confirm),
    SHELL_CMD(rollback, NULL, "Mark the running image invalid and restart into the other slot",
              cmd_ota_rollback),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ota, &sub_ota, "Firmware update slots (OTA)", NULL);
