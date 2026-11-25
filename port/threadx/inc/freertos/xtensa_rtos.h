/*
 * esp_system/port/arch/xtensa/panic_handler_asm.S asks for the RTOS glue
 * header under its ESP-IDF FreeRTOS path.  Hand it the ThreadX one.
 */
#include "../xtensa_rtos.h"
