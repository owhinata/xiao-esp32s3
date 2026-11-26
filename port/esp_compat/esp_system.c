/*
 * Stand-ins for the parts of ESP-IDF's system and flash OS code that the OTA
 * sources (app_update's esp_ota_ops.c, esp_partition's partition_target.c)
 * call and that are not built here.
 *
 * esp_restart() (esp_system's esp_system.c): the restart after
 * esp_ota_mark_app_invalid_rollback_and_reboot().  ESP-IDF runs the
 * registered shutdown handlers first; no compiled source registers one
 * (esp_register_shutdown_handler() is not built), so this is the board's
 * restart, as the shell's "reboot" does it.
 *
 * esp_flash_register_partition_ops() and
 * esp_flash_set_dangerous_write_protection() (spi_flash's
 * spi_flash_os_func_app.c, ESP-IDF's OS functions of the flash driver,
 * which port/board_flash_os.c takes the place of): ESP-IDF's OS functions
 * keep the partition API's checks of a write (not into a read-only
 * partition, not into the running application or below the end of the
 * partition table) and a switch that lets the OTA code rewrite the
 * second-stage bootloader and the partition table.  Here
 * port/board_flash_os.c lets a write or erase through only inside the
 * regions port/board_flash.c opens ("settings", otadata and, during an
 * update, the OTA slot that is not running; never the bootloader, the
 * partition table or the running application), so there
 * is nothing to register and no switch to set: both succeed without doing
 * anything, and a request to rewrite the bootloader or the table is
 * refused there.
 */
#include <stdbool.h>

#include "esp_err.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_private/esp_flash_internal.h"

#include "board.h"

void esp_restart(void)
{
    board_restart();
}

esp_err_t esp_flash_register_partition_ops(esp_flash_t *chip, esp_flash_partition_ops_t *ops)
{
    (void) chip;
    (void) ops;
    return ESP_OK;
}

esp_err_t esp_flash_set_dangerous_write_protection(esp_flash_t *chip, const bool protect)
{
    (void) chip;
    (void) protect;
    return ESP_OK;
}
