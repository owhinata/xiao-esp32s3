/*
 * Stand-in for the part of ESP-IDF's nvs_flash (not built; its headers come
 * from ESP-IDF, lib/esp-idf) that the compiled sources refer to: the
 * Wi-Fi driver's OS adapter puts these functions into its table
 * (esp_wifi/esp32s3/esp_adapter.c, g_wifi_osi_funcs).
 *
 * The driver calls them only when it keeps its configuration in NVS, which
 * is off here (CONFIG_ESP_WIFI_NVS_ENABLED, so WIFI_INIT_CONFIG_DEFAULT()
 * has nvs_enable = 0; port/net/wifi.c also sets WIFI_STORAGE_RAM).  Should
 * one be called anyway, it answers as nvs_flash does before
 * nvs_flash_init(): ESP_ERR_NVS_NOT_INITIALIZED, which the driver treats
 * as "nothing stored".
 */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    (void) namespace_name;
    (void) open_mode;
    (void) out_handle;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

void nvs_close(nvs_handle_t handle)
{
    (void) handle;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void) handle;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void) handle;
    (void) key;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_set_i8(nvs_handle_t handle, const char *key, int8_t value)
{
    (void) handle;
    (void) key;
    (void) value;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_get_i8(nvs_handle_t handle, const char *key, int8_t *out_value)
{
    (void) handle;
    (void) key;
    (void) out_value;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void) handle;
    (void) key;
    (void) value;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    (void) handle;
    (void) key;
    (void) out_value;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    (void) handle;
    (void) key;
    (void) value;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out_value)
{
    (void) handle;
    (void) key;
    (void) out_value;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void) handle;
    (void) key;
    (void) value;
    (void) length;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    (void) handle;
    (void) key;
    (void) out_value;
    (void) length;
    return ESP_ERR_NVS_NOT_INITIALIZED;
}
