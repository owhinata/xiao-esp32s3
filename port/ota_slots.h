/*
 * The application's OTA slots as ESP-IDF's OTA code (app_update) and the
 * partition table on the board describe them (port/ota_slots.c), for the
 * shell's "ota" (shell/port/ota_shell.c).
 *
 * No Espressif type appears here: the shell's sources include this header
 * and must not see sdkconfig.h.
 */
#ifndef OTA_SLOTS_H
#define OTA_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

#define OTA_SLOTS_MAX 2

struct ota_slot {
    char label[17];         /* the partition's name, "ota_0" */
    uint32_t offset;        /* flash offset */
    uint32_t size;          /* bytes */
    /*
     * The otadata entry that selects this slot, the one with the higher
     * sequence number if both do (an entry counts when its CRC agrees;
     * its state may still say not to boot it).
     */
    bool selected;
    uint32_t seq;
    const char *state;      /* "NEW", "PENDING_VERIFY", "VALID", "INVALID", "ABORTED", "UNDEFINED" */
    /* The image's application description (esp_app_desc_t), if the slot holds one. */
    bool has_image;
    char version[32];
    char date[16];
    char time[16];
};

struct ota_status {
    unsigned int count;     /* OTA slots in the partition table, up to OTA_SLOTS_MAX */
    int running;            /* index of the slot this image runs from, or -1 */
    int boot;               /* index of the slot the bootloader picks next, or -1 */
    bool otadata;           /* whether otadata could be read */
    struct ota_slot slot[OTA_SLOTS_MAX];
};

/*
 * Fills *st from the partition table, otadata and the slots' images in
 * flash (reads only).  From threads.  0, or -ENODEV when the partition
 * table has no OTA slot.
 */
int ota_slots_status(struct ota_status *st);

#endif
