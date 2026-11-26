/*
 * The OTA slots, otadata and the slots' images, read through ESP-IDF's
 * partition API (esp_partition) and OTA API (app_update), and the update
 * of the slot that is not running through the same OTA API; see
 * port/ota_slots.h.
 *
 * otadata holds two entries, one per sector: a sequence number, a state
 * and the CRC of the sequence number.  An entry with a sequence number n
 * (not 0xFFFFFFFF, CRC agreeing) selects slot (n - 1) % count, and the
 * bootloader boots the slot of the valid entry with the highest number
 * (bootloader_common_get_active_otadata()).  The entries are read here as
 * esp_ota_ops.c reads them, with the same CRC (bootloader_common).
 *
 * The update writes the slot that esp_ota_get_next_update_partition()
 * names, never the running one: esp_ota_begin() refuses that, and the
 * flash driver lets writes through only in the region that
 * board_flash_ota_slot_open() opens, which port/board_flash.c checks
 * against the running partition it found at start-up.  The region is open
 * from the first write to the end of the update.
 *
 * esp_ota_ops.c keeps no lock of its own.  Its calls that write otadata
 * (esp_ota_begin()'s invalidation of the slot's entry,
 * esp_ota_set_boot_partition(), the confirmation and the rollback) take
 * this file's lock, one caller at a time; it is held for one such call
 * (an otadata sector's erase and write, and for the switch the image's
 * check before it), never across a transfer.  The state of the update is
 * written by the one thread that runs it (the OTA service) and read
 * without the lock by ota_slots_status().
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tx_api.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_flash_partitions.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_private/startup_internal.h"
#include "bootloader_common.h"
#include "spi_flash_mmap.h"

#include "board_flash.h"
#include "ota_slots.h"

/* Where the application description lies in an image: after its header and first segment's header. */
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

_Static_assert(APP_DESC_OFFSET + sizeof(esp_app_desc_t) == 288, "ota_slots.h names 288 bytes");

static TX_MUTEX lock;

static struct {
    volatile bool active;           /* ota_update_begin() .. the end */
    bool begun;                     /* esp_ota_begin() succeeded, handle is valid */
    const esp_partition_t *slot;
    esp_ota_handle_t handle;
    uint32_t size;
    volatile uint32_t written;
} update;

/* The lock, before the first thread (ESP-IDF's secondary start-up stage, from tx_application_define()). */
ESP_SYSTEM_INIT_FN(ota_slots_lock, SECONDARY, BIT(0), 140)
{
    return tx_mutex_create(&lock, "ota", TX_NO_INHERIT) == TX_SUCCESS ? ESP_OK : ESP_FAIL;
}

static const char *state_name(uint32_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:
        return "VALID";
    case ESP_OTA_IMG_INVALID:
        return "INVALID";
    case ESP_OTA_IMG_ABORTED:
        return "ABORTED";
    default:
        return "UNDEFINED";
    }
}

/* The state of p's otadata entry, "NONE" without one. */
static const char *partition_state(const esp_partition_t *p)
{
    esp_ota_img_states_t state;

    return p != NULL && esp_ota_get_state_partition(p, &state) == ESP_OK ? state_name(state) : "NONE";
}

static void copy_field(char *dst, size_t size, const char *src, size_t src_size)
{
    size_t n = strnlen(src, src_size);

    if (n >= size) {
        n = size - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* The first 16 hex digits of a SHA-256. */
static void sha_prefix(char dst[OTA_SHA_PREFIX_SIZE], const uint8_t *sha)
{
    static const char digits[] = "0123456789abcdef";

    for (unsigned int i = 0; i < OTA_SHA_PREFIX_BYTES; i++) {
        dst[2u * i] = digits[sha[i] >> 4];
        dst[2u * i + 1u] = digits[sha[i] & 0xfu];
    }
    dst[OTA_SHA_PREFIX_SIZE - 1u] = '\0';
}

/* The two otadata entries, into the slots they select. */
static bool read_otadata(struct ota_status *st)
{
    const esp_partition_t *otadata =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);

    if (otadata == NULL) {
        return false;
    }
    for (unsigned int i = 0; i < 2u; i++) {
        esp_ota_select_entry_t entry;
        struct ota_slot *slot;

        if (esp_partition_read(otadata, i * SPI_FLASH_SEC_SIZE, &entry, sizeof(entry)) != ESP_OK) {
            return false;
        }
        if (entry.ota_seq == UINT32_MAX || entry.ota_seq == 0u
            || entry.crc != bootloader_common_ota_select_crc(&entry)) {
            continue;
        }
        slot = &st->slot[(entry.ota_seq - 1u) % st->count];
        if (!slot->selected || entry.ota_seq > slot->seq) {
            slot->selected = true;
            slot->seq = entry.ota_seq;
            slot->state = state_name(entry.ota_state);
        }
    }
    return true;
}

int ota_slots_status(struct ota_status *st)
{
    const esp_partition_t *running;
    const esp_partition_t *boot;
    const esp_partition_t *updating;
    unsigned int count = esp_ota_get_app_partition_count();

    memset(st, 0, sizeof(*st));
    st->running = -1;
    st->boot = -1;
    if (count == 0u) {
        return -ENODEV;
    }
    st->count = count < OTA_SLOTS_MAX ? count : OTA_SLOTS_MAX;
    running = esp_ota_get_running_partition();
    boot = esp_ota_get_boot_partition();
    st->running_state = partition_state(running);

    for (unsigned int i = 0; i < st->count; i++) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            ESP_PARTITION_SUBTYPE_APP_OTA_MIN + i, NULL);
        struct ota_slot *slot = &st->slot[i];
        esp_app_desc_t desc;

        if (p == NULL) {
            continue;
        }
        copy_field(slot->label, sizeof(slot->label), p->label, sizeof(p->label));
        slot->offset = p->address;
        slot->size = p->size;
        if (p == running) {
            st->running = (int) i;
        }
        if (p == boot) {
            st->boot = (int) i;
        }
        if (esp_ota_get_partition_description(p, &desc) == ESP_OK) {
            slot->has_image = true;
            copy_field(slot->version, sizeof(slot->version), desc.version, sizeof(desc.version));
            copy_field(slot->date, sizeof(slot->date), desc.date, sizeof(desc.date));
            copy_field(slot->time, sizeof(slot->time), desc.time, sizeof(desc.time));
            sha_prefix(slot->elf_sha256, desc.app_elf_sha256);
        }
    }
    st->otadata = read_otadata(st);

    updating = update.slot;
    if (update.active && updating != NULL) {
        st->updating = true;
        copy_field(st->update_slot, sizeof(st->update_slot), updating->label, sizeof(updating->label));
        st->update_size = update.size;
        st->update_written = update.written;
    }
    return 0;
}

int ota_update_begin(uint32_t size, char label[OTA_LABEL_SIZE])
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state;
    int ret = 0;

    (void) tx_mutex_get(&lock, TX_WAIT_FOREVER);
    if (update.active) {
        ret = -EBUSY;
    } else if (slot == NULL || slot == running) {
        ret = -ENODEV;
    } else if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ret = -EPERM;
    } else if (size == 0u || size > slot->size) {
        ret = -EFBIG;
    } else {
        update.slot = slot;
        update.size = size;
        update.written = 0;
        update.begun = false;
        update.active = true;
        copy_field(label, OTA_LABEL_SIZE, slot->label, sizeof(slot->label));
    }
    (void) tx_mutex_put(&lock);
    return ret;
}

/* The checks of the image's first bytes (see ota_update_write()). */
static int check_image(const uint8_t *data, uint32_t len)
{
    esp_image_header_t header;
    esp_app_desc_t desc;

    if (len < APP_DESC_OFFSET + sizeof(desc)) {
        return -ENOEXEC;
    }
    memcpy(&header, data, sizeof(header));
    memcpy(&desc, data + APP_DESC_OFFSET, sizeof(desc));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return -ENOEXEC;
    }
    if (header.hash_appended != 1u) {
        return -ENOTSUP;
    }
    if (strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) != 0) {
        return -EXDEV;
    }
    return 0;
}

int ota_update_write(const void *data, uint32_t len)
{
    if (!update.active || len > update.size - update.written) {
        return -EINVAL;
    }
    if (!update.begun) {
        esp_err_t err;
        int ret = check_image(data, len);

        if (ret != 0) {
            return ret;
        }
        if (board_flash_ota_slot_open(update.slot->address, update.slot->size) != 0) {
            return -EIO;
        }
        (void) tx_mutex_get(&lock, TX_WAIT_FOREVER);
        err = esp_ota_begin(update.slot, OTA_WITH_SEQUENTIAL_WRITES, &update.handle);
        (void) tx_mutex_put(&lock);
        if (err != ESP_OK) {
            board_flash_ota_slot_close();
            return -EIO;
        }
        update.begun = true;
    }
    if (esp_ota_write(update.handle, data, len) != ESP_OK) {
        return -EIO;
    }
    update.written += len;
    return 0;
}

static void update_end(void)
{
    board_flash_ota_slot_close();
    update.begun = false;
    update.active = false;
}

void ota_update_abort(void)
{
    if (!update.active) {
        return;
    }
    if (update.begun) {
        (void) esp_ota_abort(update.handle);
    }
    update_end();
}

int ota_update_finish(void)
{
    esp_err_t err;

    if (!update.active) {
        return -EINVAL;
    }
    if (!update.begun || update.written != update.size) {
        ota_update_abort();
        return -EINVAL;
    }
    /* esp_ota_end() releases the handle whatever it returns. */
    err = esp_ota_end(update.handle);
    update.begun = false;
    board_flash_ota_slot_close();
    if (err == ESP_OK) {
        (void) tx_mutex_get(&lock, TX_WAIT_FOREVER);
        err = esp_ota_set_boot_partition(update.slot);
        (void) tx_mutex_put(&lock);
    }
    update_end();
    return err == ESP_OK ? 0 : err == ESP_ERR_OTA_VALIDATE_FAILED ? -EBADMSG : -EIO;
}

int ota_confirm(const uint8_t *expected, struct ota_confirm *result)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const uint8_t *sha = esp_app_get_description()->app_elf_sha256;
    esp_ota_img_states_t state;
    int ret = 0;

    memset(result, 0, sizeof(*result));
    if (running == NULL || running->type != ESP_PARTITION_TYPE_APP
        || running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN
        || running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        return -ENODEV;
    }
    copy_field(result->label, sizeof(result->label), running->label, sizeof(running->label));
    sha_prefix(result->elf_sha256, sha);

    (void) tx_mutex_get(&lock, TX_WAIT_FOREVER);
    if (expected != NULL && memcmp(expected, sha, OTA_SHA_PREFIX_BYTES) != 0) {
        ret = -ESRCH;
    } else if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            result->changed = true;
        } else {
            ret = -EIO;
        }
    }
    result->state = partition_state(running);
    (void) tx_mutex_put(&lock);
    return ret;
}

bool ota_rollback_possible(void)
{
    return esp_ota_check_rollback_is_possible();
}

int ota_rollback(void)
{
    int ret;

    (void) tx_mutex_get(&lock, TX_WAIT_FOREVER);
    if (update.active) {
        ret = -EBUSY;
    } else if (!esp_ota_check_rollback_is_possible()) {
        ret = -ENOENT;
    } else {
        (void) esp_ota_mark_app_invalid_rollback_and_reboot();
        ret = -EIO;
    }
    (void) tx_mutex_put(&lock);
    return ret;
}
