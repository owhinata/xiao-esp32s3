/*
 * The registration of lwIP's sockets with ESP-IDF's VFS, which lwIP's
 * sys_init() calls (port/freertos/sys_arch.c, arch/vfs_lwip.h), in place
 * of ESP-IDF's vfs_lwip.c: there is no VFS here, the sockets are reached
 * by their lwip_*() names only, so there is nothing to register.
 */
#include "arch/vfs_lwip.h"

void esp_vfs_lwip_sockets_register(void)
{
}
