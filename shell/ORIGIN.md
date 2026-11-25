# Where the shell comes from

`shell/common/` is a copy of part of
[rp2040-connect](https://github.com/owhinata/rp2040-connect) at commit
`72f3485`: the files of its `common/` directory that this firmware compiles or
includes, at the same relative paths.  Nothing else of that repository is
here (no driver, credentials store, zperf, time synchronization, sensor,
Pico SDK or test file, nor the RP2040's flash part of the key-value store
(`chip/`, which this board's `flash_kv_os.c` replaces) and the shared
application start (`imu/app_main.c`, whose call of the store `src/main.c`
makes), nor `compat/net_offload/zephyr_net_init.c`, whose
start-up this board's `zephyr_net_wifi.c` does), and not its
`compat/include/fault_record.h`, whose record is laid out for a Cortex-M:
this board has its own in `shell/port/`.

`shell/port/` is written for this board and is not a copy.

To compare with the original (only the modified files below should differ):

```sh
diff -r shell/common /path/to/rp2040-connect/common | grep -v '^Only in /path/to'
```

## Copied files

All 120 files are byte-identical to the original, except the ones below:

| File | Change |
|---|---|
| `compat/include/priorities.h` | `APP_STACK_SHELL` is wrapped in `#ifndef`, as the file already does for `APP_STACK_SENSOR_SHELL` and three others, so that `shell/port/priorities_os.h` can set the shell thread's stack for this CPU. |
| `shell/kernel_shell.c` | Texts that named RP2040 specifics now name this board's: the help of `id` (the factory MAC address instead of the flash's ID), the clock in `uptime` (SYSTIMER instead of TIMER), and the help and message of `bootsel` (the ROM download mode instead of BOOTSEL and the RPI-RP2 drive), with the two comments that explain them. No code change. |
| `compat/log/log_ring.c` | The log time stamp is `[HH:MM:SS.mmmuuu]`, without the comma Zephyr puts between the milliseconds and the microseconds: one statement removed. The comments that quote the format are changed to match in `compat/log/log_ram.c`, `compat/internal/log_ring.h` and `compat/include/app_log.h`. |
| `compat/include/flash_kv.h` | The key-value area is this board's `settings` partition (`partitions.csv`): `FLASH_KV_FLASH_SIZE` 8 MiB, `FLASH_KV_OFFSET` 0x9000, `FLASH_KV_SIZE` 0x7000 (seven sectors) instead of the last 8 KiB of the RP2040's 16 MiB flash, and the `#error` checks what this layout needs (whole 4 KiB sectors, at least two, below the application at 0x10000) instead of "the last two sectors". The first lines of the header comment and the comment on the page and sector size (which named the RP2040 board's flash chip) say the same. The build checks the constants against the partition table (`shell/port/flash_kv_os.c`). The on-flash format (NVS, write block 1, 4 KiB sectors) is unchanged. |
| `compat/zephyr_flash_kv.c` | The flash controller's device name is `flash-controller@60002000` (SPI1 of the ESP32-S3, as Zephyr's devicetree for this chip names it) instead of the RP2040's `flash-controller@18000000`. A string only; the comments still describe the Zephyr build of rp2040-connect, whose format the store keeps. |

By licence, as each file's `SPDX-License-Identifier` line says (paths
relative to `shell/common/`; headers without a directory are in
`compat/include/`):

- **MIT** (rp2040-connect's own; Copyright (c) 2025 KatsumiOuwa): the shell
  engine with its Telnet shell (`compat/shell_engine/` except
  `shell_argv.c`), the generic kernel
  part and its ThreadX primitives (`compat/kernel_generic/`,
  `compat/port/threadx/`), the boundary headers and the prelude
  (`compat/port/*.h`, `compat/port/generic/`, `shell/port/`),
  `shell/kernel_shell.c`, `shell/dmesg_shell.c`, `shell/ifconfig_shell.c`,
  the store's flash device and `kv wipe` (`compat/zephyr_flash_kv.c`,
  `compat/kv_store_wipe.c`, `kv_store_os.h`, `zephyr_kv_config.h`,
  `zephyr/drivers/flash.h`, `zephyr/storage/flash_map.h`, `zephyr/sys/crc.h`,
  `zephyr/types.h`),
  `shell/include/ps_interval.h`, the POSIX network headers newlib lacks and
  `inet_pton()`/`inet_ntop()` (`compat/libc_net/`), the network interface
  and the socket layer's headers of its own (`compat/net_offload/zephyr_net_if.c`,
  `compat/net_offload/include/zephyr/net/net_if.h`, `net_ip.h`,
  `socket.h`), and the headers `shell_console.h`,
  `shell_engine.h`, `shell_serial.h`, `shell_telnet.h`, `zephyr_compat_os.h`,
  `zephyr_heap.h`, `zephyr/logging/log.h`, `zephyr/toolchain.h`,
  `internal/cli.h`, `internal/shell_argv.h`, `internal/zephyr_os_internal.h`.
- **Apache-2.0** (derived from Zephyr): `shell/zshell.c`, `shell/subcmd.c`,
  `shell/include/subcmd.h`, `compat/shell_engine/shell_argv.c`,
  `compat/port/cpu_load.h`,
  `compat/log/log_ram.c`, `compat/log/log_ring.c`, `internal/log_ring.h`,
  `compat/lib/utils/getopt/getopt_common.h`, the `wifi` command and the
  network management layer under it (`compat/zephyr_wifi_shell.c`,
  `compat/zephyr_wifi_mgmt.c`, `compat/zephyr_wifi_utils.c`,
  `compat/zephyr_net_mgmt.c`, `zephyr/net/net_mgmt.h`,
  `zephyr/net/wifi_mgmt.h`, `zephyr/net/wifi_utils.h`), Zephyr's socket layer
  (`compat/net_offload/subsys/net/lib/sockets/sockets.c`,
  `socket_offload.c`, `getaddrinfo.c`, `compat/net_offload/lib/os/zvfs/zvfs_fdtable.c`,
  `zvfs_poll.c`, `compat/net_offload/include/zephyr/net/socket_offload.h`,
  `compat/net_offload/include/zephyr/sys/fdtable.h`), Zephyr's settings,
  NVS and CRC-8 (`compat/subsys/settings/src/settings.c`, `settings_store.c`,
  `settings_init.c`, `settings_nvs.c`, `settings_priv.h`,
  `compat/subsys/settings/include/settings/settings_nvs.h`, `settings_file.h`,
  `compat/subsys/kvss/nvs/nvs.c`, `nvs_priv.h`, `compat/subsys/crc/crc8_sw.c`,
  `zephyr/settings/settings.h`, `zephyr/kvss/nvs.h`), the store and `kv`
  (`imu/kv_store.c`, `imu/kv_store.h`, `flash_kv.h`,
  `compat/port/flash_kv_os.h`, `compat/lib/utils/hex.c`), and the headers
  `app_log.h`, `log_ram.h`, `priorities.h`, `zephyr_compat.h` and `zephyr/`
  (`device.h`, `devicetree.h`, `drivers/hwinfo.h`, `kernel.h`,
  `shell/shell.h`, `spinlock.h`, `sys/atomic.h`, `sys/iterable_sections.h`,
  `sys/list_gen.h`, `sys/printk.h`, `sys/slist.h`, `sys/sys_getopt.h`,
  `sys/util.h`).
- **BSD-3-Clause** (through Zephyr, from NetBSD and OpenBSD):
  `compat/lib/utils/getopt/getopt.c`, `getopt_long.c`,
  `compat/subsys/portability/posix/c_lib_ext/fnmatch.c`,
  `compat/include/zephyr/posix/fnmatch.h`.

Some of the copied files are compiled or included for a part only, and the
rest of each is dropped at link time or never expanded:

- `compat/port/cpu_load.h` gives `ps` the arithmetic of `cpu_active`
  (`cpu_load_ratio()`); `cpu_load_get()`, which it declares, is not
  defined here.
- `shell/zshell.c` holds Zephyr's device-name helpers, which no command here
  calls.
- `compat/port/generic/common_prelude.h` gives the shared sources the
  Zephyr build's Kconfig values; of them only
  `CONFIG_NET_SOCKETS_OFFLOAD_PRIORITY` is used here (by
  `shell/port/zephyr_net_socket.c`).
- `compat/net_offload/subsys/net/lib/sockets/sockets.c` has `zsock_recvmsg()`,
  which the board's sockets do not implement (`EOPNOTSUPP`); the socket
  layer's own poll in `zvfs_poll.c` (`k_poll()` on semaphores) is linked
  but not taken, as every socket here is offloaded.
- `compat/zephyr_wifi_mgmt.c` and `zephyr/net/wifi_mgmt.h` have the stored
  credentials' parts (`CONFIG_WIFI_CREDENTIALS`, not defined here);
  `compat/zephyr_wifi_utils.c` has `net_bytes_from_str()`, which nothing
  calls.
- Of Zephyr's settings only the NVS backend and the calls of the store are
  used (`settings_file.h` is included, its file backend is not built);
  `compat/zephyr_flash_kv.c` also recognizes the NuttX build's earlier
  format, which an area on this board never holds (it reports it like any
  other foreign data).

## This board's part (`shell/port/`)

| File | The boundary it implements |
|---|---|
| `zephyr_os_port.h` | What `compat/port/threadx/zephyr_kernel_os_impl.h` takes from the ThreadX port and the board: interrupt context, interrupt lockout, busy wait, microsecond clock, assertion |
| `priorities_os.h` | Priority mapping, time slices, the stacks of the shell's threads (the console's and Telnet's session), the network event thread, `wifi_evt` and `telnet_service` (`compat/include/priorities.h`) |
| `shell_serial_os_impl.h` | Console transport and stdio redirection (`compat/port/shell_serial_os.h`) |
| `log_ram_os_impl.h` | RAM log: retained section, lock, clock (`compat/port/log_ram_os.h`) |
| `kernel_shell_os_impl.h` | Tick count, restart, download mode, the record for `fault` (`shell/port/kernel_shell_os.h`) |
| `fault_record.h`, `fault_record.c` | The record of a fatal error that `port/panic.c` keeps across the restart (with this board's reasons: abort, failed assertion, failed `ESP_ERROR_CHECK()`), for an Xtensa exception frame: the counterpart of rp2040-connect's `compat/include/fault_record.h` and `compat/log/fault_record.c`, with the same name so that `shell/kernel_shell.c` includes it (`shell/port` is first on the include path); its text for `fault`, the banner and the RAM log, and taking it at the start |
| `shell_port.c` | `k_uptime_get()` (rp2040-connect's is in `compat/zephyr_init.c`, not copied), `hwinfo_get_device_id()`, the layer's tick (`zephyr_compat_tick()`) |
| `zephyr_net_wifi.c`, `zephyr_net_wifi.h` | ESP-IDF's Wi-Fi driver with lwIP (`port/net/wifi.h`) as the offloaded Wi-Fi interface (`struct wifi_mgmt_ops`: scan, connect, disconnect, status), the counterpart of rp2040-connect's `drivers/nina/port/zephyr/nina_net.c`: the interface's address and state (`compat/net_offload/zephyr_net_if.c`) on the driver's notifications, and the thread `wifi_evt` that raises the layer's events one at a time, at the priority of the NINA's Wi-Fi thread (`APP_PRIO_NINA_WIFI`); the start-up of the network event thread, the interface and the `wifi` command (`zephyr_net_initialize()`, as rp2040-connect's `compat/net_offload/zephyr_net_init.c`) |
| `zephyr_net_socket.c`, `zephyr_net_socket.h` | lwIP's sockets as the socket layer's offloaded sockets (`NET_SOCKET_OFFLOAD_REGISTER()`, `struct socket_op_vtable` with poll offloaded to `lwip_poll()`) and its resolver (`socket_offload_dns_register()`), the counterpart of rp2040-connect's `drivers/nina/port/zephyr/nina_socket.c`: the layer's side, with the values of `compat/libc_net`; lwIP's side, with lwIP's values, is `port/net/lwip_bridge.c` behind the neutral `port/net/lwip_bridge.h` |
| `zephyr_sys_clock.c` | `sys_clock_gettime()`/`sys_clock_settime()` (`compat/port/generic/zephyr/sys/clock.h`) on the SYSTIMER uptime, as rp2040-connect's `compat/port/pico_sdk/zephyr_sys_clock.c` on the RP2040 TIMER; newlib's `_gettimeofday_r()` and `settimeofday()` on the same clock |
| `flash_kv_os.c` | The key-value area's flash operations (`compat/port/flash_kv_os.h`) on the `settings` partition, through the flash driver (`port/board_flash.h`): the second of the three range checks, the bounds set only once the partition table on the board agrees, a page program trimmed to its bytes that are not 0xff; the counterpart of rp2040-connect's `common/chip/rp2040/rp2040_flash_kv.c` |
| `commands.c` | `version`, `free`, `ps` (with the run times of ThreadX's execution profile kit) and the command table `shell_engine_cmds[]` |
