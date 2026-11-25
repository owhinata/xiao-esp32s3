/*
 * Between port/board_flash.c (the driver's start-up and the calls, in
 * flash) and port/board_flash_os.c (its OS functions, in RAM).
 */
#ifndef BOARD_FLASH_OS_H
#define BOARD_FLASH_OS_H

#include "esp_flash.h"
#include "esp_flash_chips/esp_flash_types.h"

/* The table that esp_flash_default_chip uses once the driver has started. */
extern const esp_flash_os_functions_t board_flash_os_functions;

/* From then on writes and erases inside the "settings" partition are let through. */
void board_flash_os_allow_settings(void);

#endif
