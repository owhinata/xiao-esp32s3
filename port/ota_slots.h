/*
 * The application's OTA slots as ESP-IDF's OTA code (app_update) and the
 * partition table on the board describe them, and the update of the slot
 * that is not running (port/ota_slots.c), for the shell's "ota"
 * (shell/port/ota_shell.c) and the OTA service (shell/port/ota_service.c).
 *
 * No Espressif type appears here: the shell's sources include this header
 * and must not see sdkconfig.h.
 *
 * The functions are for threads.  The ones that write otadata (the end of
 * an update, the confirmation, the rollback) take one caller at a time;
 * none of them waits for a transfer, which writes only the slot.
 */
#ifndef OTA_SLOTS_H
#define OTA_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

#define OTA_SLOTS_MAX 2
#define OTA_LABEL_SIZE 17           /* a partition's name with its NUL */
#define OTA_SHA_PREFIX_SIZE 17      /* 16 hex digits of a SHA-256 with the NUL */

struct ota_slot {
    char label[OTA_LABEL_SIZE];     /* the partition's name, "ota_0" */
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
    char elf_sha256[OTA_SHA_PREFIX_SIZE];   /* the first 16 hex digits of the ELF file's SHA-256 */
};

struct ota_status {
    unsigned int count;     /* OTA slots in the partition table, up to OTA_SLOTS_MAX */
    int running;            /* index of the slot this image runs from, or -1 */
    const char *running_state;  /* its otadata entry's state (as slot[].state), "NONE" without one */
    int boot;               /* index of the slot the bootloader picks next, or -1 */
    bool otadata;           /* whether otadata could be read */
    struct ota_slot slot[OTA_SLOTS_MAX];
    /* A transfer in flight (ota_update_begin() .. its end). */
    bool updating;
    char update_slot[OTA_LABEL_SIZE];
    uint32_t update_size;   /* the image's size, bytes */
    uint32_t update_written;    /* of it written so far */
};

/*
 * Fills *st from the partition table, otadata and the slots' images in
 * flash (reads only).  0, or -ENODEV when the partition table has no OTA
 * slot.
 */
int ota_slots_status(struct ota_status *st);

/*
 * An update of the OTA slot that is not running, one at a time:
 *
 * ota_update_begin(): an image of size bytes for that slot; its name into
 * label.  Checks only, nothing is written.  0, -ENODEV (no OTA slot to
 * write), -EBUSY (an update is in flight), -EPERM (this image is still
 * pending verification: confirm it or roll back first), -EFBIG (size is 0
 * or more than the slot holds).
 *
 * ota_update_write(): the image's next len bytes, in order.  The first
 * call must hold at least the image's headers and application description
 * (288 bytes); it is checked to be an application image with its SHA-256
 * appended, of the project this image is (its name in the description),
 * before anything is written.  Then the slot's write permission is opened
 * and ESP-IDF's esp_ota_begin() run (with rollback it also invalidates the
 * otadata entry that selects the slot, if any).  Each sector is erased
 * when the image reaches it.  0, -ENOEXEC (not an application image),
 * -ENOTSUP (no SHA-256 appended), -EXDEV (another project), -EINVAL (no
 * update begun, or more than its size), -EIO (the flash refused or
 * failed).
 *
 * ota_update_finish(): once all of size is written: checks the image as
 * written (esp_ota_end(): ESP-IDF's esp_image_verify(), with the SHA-256)
 * and makes the slot the one to boot next (esp_ota_set_boot_partition(),
 * which writes otadata).  The update ends either way.  0, -EINVAL (not
 * all written), -EBADMSG (the image does not check out), -EIO (otadata
 * was not written).
 *
 * ota_update_abort(): ends an update in flight without switching
 * (esp_ota_abort()); nothing if none is.
 *
 * In every case the slot's write permission is closed when the update
 * ends.
 */
int ota_update_begin(uint32_t size, char label[OTA_LABEL_SIZE]);
int ota_update_write(const void *data, uint32_t len);
int ota_update_finish(void);
void ota_update_abort(void);

/*
 * The confirmation of the running image: when its otadata entry is
 * PENDING_VERIFY (the first start after an update), marks it VALID
 * (esp_ota_mark_app_valid_cancel_rollback()); otherwise changes nothing.
 * With expected (the first OTA_SHA_PREFIX_BYTES bytes of an ELF SHA-256),
 * only if this image's ELF SHA-256 begins with them.  Fills the running
 * slot's name, its state afterwards and the first 16 hex digits of this
 * image's ELF SHA-256.  0, -ENODEV (not running from an OTA slot), -ESRCH
 * (another image runs; nothing changed), -EIO (otadata was not written).
 */
#define OTA_SHA_PREFIX_BYTES ((OTA_SHA_PREFIX_SIZE - 1) / 2)

struct ota_confirm {
    char label[OTA_LABEL_SIZE];
    const char *state;
    bool changed;           /* it was PENDING_VERIFY and is now VALID */
    char elf_sha256[OTA_SHA_PREFIX_SIZE];
};

int ota_confirm(const uint8_t *expected, struct ota_confirm *result);

/*
 * The rollback: whether the other slot holds an image that the bootloader
 * would boot (esp_ota_check_rollback_is_possible()), and the rollback
 * itself, which marks the running image INVALID and restarts
 * (esp_ota_mark_app_invalid_rollback_and_reboot()); it returns only when
 * it does not do that: -EBUSY (an update is in flight), -ENOENT (no image
 * to go back to), -EIO.
 */
bool ota_rollback_possible(void);
int ota_rollback(void);

#endif
