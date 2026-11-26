# Where the shell comes from

`shell/common/` is a copy of part of
[rp2040-connect](https://github.com/owhinata/rp2040-connect) at commit
`72f3485`: the files of its `common/` directory that this firmware compiles or
includes, at the same relative paths.  Nothing else of that repository is
here (no driver, sensor, Pico SDK or test file, nor the RP2040's part of
the key-value store and its timer (`chip/`, which this board's
`flash_kv_os.c` and `rp2040_timer_us.h` replace) and the shared
application start (`imu/app_main.c` and `imu/app_init.c`: `src/main.c`
makes their call of the store, and this board's `zephyr_net_wifi.c` their
start of the Wi-Fi credentials, the time synchronization, the Wi-Fi
profile, and their threads with that of the iperf3 server), nor
`compat/net_offload/zephyr_net_init.c`, whose
start-up this board's `zephyr_net_wifi.c` does), and not its
`compat/include/fault_record.h`, whose record is laid out for a Cortex-M:
this board has its own in `shell/port/`.

`shell/port/` is written for this board and is not a copy.

To compare with the original (only the modified files below should differ):

```sh
diff -r shell/common /path/to/rp2040-connect/common | grep -v '^Only in /path/to'
```

## Copied files

All 141 files are byte-identical to the original, except the ones below:

| File | Change |
|---|---|
| `compat/include/priorities.h` | `APP_STACK_SHELL`, `APP_STACK_WIFI_RECONNECT` and `APP_STACK_TIME_SYNC` are wrapped in `#ifndef`, as the file already does for `APP_STACK_SENSOR_SHELL` and three others, and an entry for the iperf3 server's thread `zperf_server_tid` is added in the file's form (`APP_PRIO_ZPERF_SERVER`, Zephyr's 14, and `APP_STACK_ZPERF_SERVER`, 2048 bytes, in `#ifndef`), so that `shell/port/priorities_os.h` can set the stacks of the shell's thread, of the Wi-Fi profile's reconnection thread, of the time synchronization's thread and of the iperf3 server's thread for this CPU. |
| `imu/wifi_profile.c` | The reconnection thread's stack is `APP_STACK_WIFI_RECONNECT` of `priorities.h` (which already names this thread and its 2048 bytes) instead of the number 2048, with `#include "priorities.h"`: on this CPU a log line on the thread's path does not fit in 2048 bytes (`shell/port/priorities_os.h`). The comment with the measurement on the RP2040 is left as it is. The NINA driver's hooks it registers (`nina_wifi.h`) are this board's header of that name, `shell/port/nina_wifi.h`, so the code is otherwise unchanged. |
| `imu/time_sync.c` | The thread's stack is `APP_STACK_TIME_SYNC` of `priorities.h` (which already names this thread and its 2048 bytes) instead of the number 2048, with `#include "priorities.h"`, as in `imu/wifi_profile.c`: on this CPU the thread's path through the socket layer to lwIP needs more (`shell/port/priorities_os.h`). The comments on the RP2040's timer are left as they are; `rp2040_timer_us()`, which it includes by that name, is this board's header `shell/port/rp2040_timer_us.h`. |
| `compat/zperf_iperf3.c` | The iperf3 server's stack is `APP_STACK_ZPERF_SERVER` of `priorities.h` instead of the number 2048, with `#include "priorities.h"`, as in `imu/wifi_profile.c`: on this CPU its poll path and its report through newlib's printf need more (`shell/port/priorities_os.h`). The comments that name the NINA's sockets are left as they are. |
| `shell/kernel_shell.c` | Texts that named RP2040 specifics now name this board's: the help of `id` (the factory MAC address instead of the flash's ID), the clock in `uptime` (SYSTIMER instead of TIMER), and the help and message of `bootsel` (the ROM download mode instead of BOOTSEL and the RPI-RP2 drive), with the two comments that explain them. No code change. |
| `compat/log/log_ring.c` | The log time stamp is `[HH:MM:SS.mmmuuu]`, without the comma Zephyr puts between the milliseconds and the microseconds: one statement removed. The comments that quote the format are changed to match in `compat/log/log_ram.c`, `compat/internal/log_ring.h` and `compat/include/app_log.h`. |
| `compat/include/flash_kv.h` | The key-value area is this board's `settings` partition (`partitions.csv`): `FLASH_KV_FLASH_SIZE` 8 MiB, `FLASH_KV_OFFSET` 0x9000, `FLASH_KV_SIZE` 0x7000 (seven sectors) instead of the last 8 KiB of the RP2040's 16 MiB flash, and the `#error` checks what this layout needs (whole 4 KiB sectors, at least two, below the application at 0x10000) instead of "the last two sectors". The first lines of the header comment and the comment on the page and sector size (which named the RP2040 board's flash chip) say the same. The build checks the constants against the partition table (`shell/port/flash_kv_os.c`). The on-flash format (NVS, write block 1, 4 KiB sectors) is unchanged. |
| `compat/wifi_credentials_backend_settings.c` | When the stored credentials are loaded, an entry whose key gives a negative index (`wifi_cred/-1`) or whose header has an SSID length above `WIFI_SSID_MAX_LEN` is logged and skipped: both were used unchecked as the index and the `memcpy()` length of the SSID cache (`wifi_credentials_cache_ssid()`), which they overran.  The file's list of changes says so. |
| `compat/wifi_credentials.c` | `wifi_credentials_get_by_ssid_personal_struct()` reports a stored entry as corrupted (`-EPROTO`) also when its SSID, password, anonymous identity or key password length exceeds its array (they are used as copy and print lengths later), and `wifi_credentials_get_by_ssid_personal()` fails with `-ENOBUFS` when the password is longer than the caller's buffer instead of copying past it.  The file's list of changes says so. |
| `compat/zephyr_net_mgmt.c` | `net_mgmt_add_event_callback()` takes a callback that is already in the list out first, as Zephyr's does (`sys_slist_find_and_remove()` before the prepend); the removal is shared with `net_mgmt_del_event_callback()`.  Adding a listed callback again linked it to itself, and the dispatch then looped for ever under the callback lock (two consoles running `wifi scan` at once). |
| `compat/zephyr_wifi_shell.c` | `wifi scan` adds its callback without removing it first, as Zephyr's `wifi_shell.c` does, and the item of the file's list of changes about that removal is gone: the layer's add now removes it itself (`compat/zephyr_net_mgmt.c`).  The separate removal and add were not one step, which let two consoles add it twice. |
| `imu/time_offset.c` | `update_rate()` takes a change of the offset that does not fit the product with 10^9 (above about 9223 s, or one whose difference overflows) as an implausible rate without computing it; the product overflowed `int64_t` (undefined behaviour) before the plausibility check. |
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
  the port's minimal iperf3 of "zperf" (`compat/zperf_iperf3.c`,
  `zephyr/net/zperf.h`, `internal/zperf_iperf3.h`),
  the store's flash device and `kv wipe` (`compat/zephyr_flash_kv.c`,
  `compat/kv_store_wipe.c`, `kv_store_os.h`, `zephyr_kv_config.h`,
  `zephyr/drivers/flash.h`, `zephyr/storage/flash_map.h`, `zephyr/sys/crc.h`,
  `zephyr/types.h`), the headers that the copy of Zephyr's
  `wifi_credentials_shell.c` includes for nothing but the log macros
  (`zephyr/init.h`, `zephyr/net/net_event.h`, `zephyr/net/net_l2.h`,
  `zephyr/net/ethernet.h`),
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
  `compat/port/flash_kv_os.h`, `compat/lib/utils/hex.c`), Zephyr's Wi-Fi
  credentials with their settings backend and `wifi cred`
  (`compat/wifi_credentials.c`, `compat/wifi_credentials_backend_settings.c`,
  `internal/wifi_credentials_internal.h`, `zephyr/net/wifi_credentials.h`,
  `compat/subsys/net/lib/wifi_credentials/wifi_credentials_shell.c`), the
  Wi-Fi profile (`imu/wifi_profile.c`, `imu/wifi_profile.h`; its OS part
  `compat/port/generic/wifi_profile_os.h` is MIT), the "zperf" command
  (`compat/zephyr_zperf_shell.c`, the TCP part of Zephyr's `zperf_shell.c`),
  the time synchronization and "ntp" (`imu/time_sync.c`, `imu/time_sync.h`,
  `imu/time_offset.c`, `imu/time_offset.h`), and the headers
  `app_log.h`, `log_ram.h`, `priorities.h`, `zephyr_compat.h` and `zephyr/`
  (`device.h`, `devicetree.h`, `drivers/hwinfo.h`, `kernel.h`,
  `shell/shell.h`, `spinlock.h`, `sys/atomic.h`, `sys/byteorder.h`,
  `sys/iterable_sections.h`, `sys/list_gen.h`, `sys/printk.h`, `sys/slist.h`,
  `sys/sys_getopt.h`, `sys/util.h`).
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
  `CONFIG_NET_SOCKETS_OFFLOAD_PRIORITY` (by `shell/port/zephyr_net_socket.c`)
  and `CONFIG_NINA_NTP_SERVER`, `_PORT` and `_INTERVAL_MS` (by
  `imu/time_sync.c`: the default gateway, port 123, every 2 s) are used
  here.
- `imu/time_sync.c` resolves the NTP server's name with `getaddrinfo()`
  only when `CONFIG_NINA_NTP_SERVER` names one, which here it does not (the
  compiler leaves the branch out); of its API (`time_sync.h`) only
  `time_sync_init()` is called: the conversions of the board's clock to the
  server's time (`time_sync_unix_us()`, `time_sync_estimate_us()`,
  `time_sync_is_ready()`), which rp2040-connect's IMU service uses, are
  dropped at link time.
- `compat/net_offload/subsys/net/lib/sockets/sockets.c` has `zsock_recvmsg()`,
  which the board's sockets do not implement (`EOPNOTSUPP`); the socket
  layer's own poll in `zvfs_poll.c` (`k_poll()` on semaphores) is linked
  but not taken, as every socket here is offloaded.
- `compat/wifi_credentials.c` has `wifi_credentials_get_by_ssid_personal()`
  and `wifi_credentials_delete_all()`, which nothing calls.  It stores a
  network as `struct wifi_credentials_personal` is in memory
  (`zephyr/net/wifi_credentials.h`, whose layout check is for Arm only):
  here `enum wifi_security_type` has 4 bytes instead of the Arm builds' 1,
  so the SSID starts at byte 4 instead of 1 (the entry is 224 bytes in
  both, the other fields at the same offsets).  The keys
  (`wifi_cred/<n>`) and the store's format are rp2040-connect's; the bytes
  of an entry are not.
- `imu/wifi_profile.c` moves an earlier format of rp2040-connect's stored
  network (the keys `app_kv/sys.wifi_profile` and `wifi_last/last`) to the
  credentials at the start; no store on this board holds one, so it finds
  nothing.
- Of Zephyr's settings only the NVS backend and the calls of the store are
  used (`settings_file.h` is included, its file backend is not built);
  `compat/zephyr_flash_kv.c` also recognizes the NuttX build's earlier
  format, which an area on this board never holds (it reports it like any
  other foreign data).

## This board's part (`shell/port/`)

| File | The boundary it implements |
|---|---|
| `zephyr_os_port.h` | What `compat/port/threadx/zephyr_kernel_os_impl.h` takes from the ThreadX port and the board: interrupt context, interrupt lockout, busy wait, microsecond clock, assertion |
| `priorities_os.h` | Priority mapping, time slices, the stacks of the shell's threads (the console's and Telnet's session), the network event thread, `wifi_evt`, the Wi-Fi profile's `wifi_reconnect_tid`, `telnet_service`, the iperf3 server's `zperf_server_tid` and the time synchronization's `time_sync_tid` (`compat/include/priorities.h`) |
| `shell_serial_os_impl.h` | Console transport and stdio redirection (`compat/port/shell_serial_os.h`) |
| `log_ram_os_impl.h` | RAM log: retained section, lock, clock (`compat/port/log_ram_os.h`) |
| `kernel_shell_os_impl.h` | Tick count, restart, download mode, the record for `fault` (`shell/port/kernel_shell_os.h`) |
| `fault_record.h`, `fault_record.c` | The record of a fatal error that `port/panic.c` keeps across the restart (with this board's reasons: abort, failed assertion, failed `ESP_ERROR_CHECK()`), for an Xtensa exception frame: the counterpart of rp2040-connect's `compat/include/fault_record.h` and `compat/log/fault_record.c`, with the same name so that `shell/kernel_shell.c` includes it (`shell/port` is first on the include path); its text for `fault`, the banner and the RAM log, and taking it at the start |
| `shell_port.c` | `k_uptime_get()` (rp2040-connect's is in `compat/zephyr_init.c`, not copied), `hwinfo_get_device_id()`, the layer's tick (`zephyr_compat_tick()`) |
| `zephyr_net_wifi.c`, `zephyr_net_wifi.h` | ESP-IDF's Wi-Fi driver with lwIP (`port/net/wifi.h`) as the offloaded Wi-Fi interface (`struct wifi_mgmt_ops`: scan, connect, disconnect, status), the counterpart of rp2040-connect's `drivers/nina/port/zephyr/nina_net.c`: the interface's address and state (`compat/net_offload/zephyr_net_if.c`) on the driver's notifications, and the thread `wifi_evt` that raises the layer's events one at a time, at the priority of the NINA's Wi-Fi thread (`APP_PRIO_NINA_WIFI`); the start-up of the network event thread, the interface and the `wifi` command (`zephyr_net_initialize()`, as rp2040-connect's `compat/net_offload/zephyr_net_init.c`); the security types of a connection that the driver can require (the rest refused); the Wi-Fi profile's hooks (`nina_wifi_set_reconnect_ops()`) handed to the driver, and the start of the stored networks, of the time synchronization, of the profile and of the iperf3 server's thread (as rp2040-connect's `imu/app_init.c` and `imu/app_main.c`) |
| `rp2040_timer_us.h` | The header that `imu/time_sync.c` includes for rp2040-connect's `common/chip/rp2040/include/rp2040_timer_us.h`: `rp2040_timer_us()`, the RP2040 TIMER there, as this board's uptime clock (`board_uptime_us()`, SYSTIMER), the clock of the layer and of the realtime clock |
| `nina_wifi.h` | The part of rp2040-connect's NINA driver header (`common/drivers/nina/include/nina_wifi.h`) that `imu/wifi_profile.c` includes: `struct nina_wifi_reconnect_ops` as there and `nina_wifi_set_reconnect_ops()`, which `zephyr_net_wifi.c` defines for this board's driver |
| `zephyr_net_socket.c`, `zephyr_net_socket.h` | lwIP's sockets as the socket layer's offloaded sockets (`NET_SOCKET_OFFLOAD_REGISTER()`, `struct socket_op_vtable` with poll offloaded to `lwip_poll()`) and its resolver (`socket_offload_dns_register()`), the counterpart of rp2040-connect's `drivers/nina/port/zephyr/nina_socket.c`: the layer's side, with the values of `compat/libc_net`; lwIP's side, with lwIP's values, is `port/net/lwip_bridge.c` behind the neutral `port/net/lwip_bridge.h` |
| `zephyr_sys_clock.c` | `sys_clock_gettime()`/`sys_clock_settime()` (`compat/port/generic/zephyr/sys/clock.h`; the time synchronization sets the realtime clock) on the SYSTIMER uptime, as rp2040-connect's `compat/port/pico_sdk/zephyr_sys_clock.c` on the RP2040 TIMER; newlib's `_gettimeofday_r()` and `settimeofday()` on the same clock |
| `flash_kv_os.c` | The key-value area's flash operations (`compat/port/flash_kv_os.h`) on the `settings` partition, through the flash driver (`port/board_flash.h`): the second of the three range checks, the bounds set only once the partition table on the board agrees, a page program trimmed to its bytes that are not 0xff; the counterpart of rp2040-connect's `common/chip/rp2040/rp2040_flash_kv.c` |
| `commands.c` | `version`, `free`, `ps` (with the run times of ThreadX's execution profile kit) and the command table `shell_engine_cmds[]` |
