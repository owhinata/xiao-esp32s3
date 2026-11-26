/*
 * The OTA slots, otadata and the slots' images, read through ESP-IDF's
 * partition API (esp_partition) and OTA API (app_update); see
 * port/ota_slots.h.
 *
 * otadata holds two entries, one per sector: a sequence number, a state
 * and the CRC of the sequence number.  An entry with a sequence number n
 * (not 0xFFFFFFFF, CRC agreeing) selects slot (n - 1) % count, and the
 * bootloader boots the slot of the valid entry with the highest number
 * (bootloader_common_get_active_otadata()).  The entries are read here as
 * esp_ota_ops.c reads them, with the same CRC (bootloader_common).
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_flash_partitions.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "bootloader_common.h"
#include "spi_flash_mmap.h"

#include "ota_slots.h"

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

static void copy_field(char *dst, size_t size, const char *src, size_t src_size)
{
    size_t n = strnlen(src, src_size);

    if (n >= size) {
        n = size - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
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
        }
    }
    st->otadata = read_otadata(st);
    return 0;
}
