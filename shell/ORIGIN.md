# Where the shell comes from

`shell/common/` is a copy of part of
[rp2040-connect](https://github.com/owhinata/rp2040-connect) at commit
`72f3485`: the files of its `common/` directory that this firmware compiles or
includes, at the same relative paths.  Nothing else of that repository is
here (no driver, socket or offloaded network stack, credentials store,
Telnet, sensor, Pico SDK or test file), and not its
`compat/include/fault_record.h`, whose record is laid out for a Cortex-M,
nor its `compat/net_offload/include/zephyr/net/net_if.h`, which takes its
address type from the socket headers not copied: this board has its own of
both in `shell/port/`.

`shell/port/` is written for this board and is not a copy.

To compare with the original (only the modified files below should differ):

```sh
diff -r shell/common /path/to/rp2040-connect/common | grep -v '^Only in /path/to'
```

## Copied files

All 68 files are byte-identical to the original, except the ones below:

| File | Change |
|---|---|
| `compat/include/priorities.h` | `APP_STACK_SHELL` is wrapped in `#ifndef`, as the file already does for `APP_STACK_SENSOR_SHELL` and three others, so that `shell/port/priorities_os.h` can set the shell thread's stack for this CPU. |
| `shell/kernel_shell.c` | Texts that named RP2040 specifics now name this board's: the help of `id` (the factory MAC address instead of the flash's ID), the clock in `uptime` (SYSTIMER instead of TIMER), and the help and message of `bootsel` (the ROM download mode instead of BOOTSEL and the RPI-RP2 drive), with the two comments that explain them. No code change. |
| `compat/log/log_ring.c` | The log time stamp is `[HH:MM:SS.mmmuuu]`, without the comma Zephyr puts between the milliseconds and the microseconds: one statement removed. The comments that quote the format are changed to match in `compat/log/log_ram.c`, `compat/internal/log_ring.h` and `compat/include/app_log.h`. |

By licence, as each file's `SPDX-License-Identifier` line says (paths
relative to `shell/common/`; headers without a directory are in
`compat/include/`):

- **MIT** (rp2040-connect's own; Copyright (c) 2025 KatsumiOuwa): the shell
  engine (`compat/shell_engine/` except `shell_argv.c`), the generic kernel
  part and its ThreadX primitives (`compat/kernel_generic/`,
  `compat/port/threadx/`), the boundary headers (`compat/port/*.h`,
  `compat/port/generic/`, `shell/port/`), `shell/kernel_shell.c`,
  `shell/dmesg_shell.c`, `shell/include/ps_interval.h`, and the headers `shell_console.h`,
  `shell_engine.h`, `shell_serial.h`, `zephyr_compat_os.h`, `zephyr_heap.h`,
  `zephyr/logging/log.h`, `zephyr/toolchain.h`, `internal/cli.h`,
  `internal/shell_argv.h`, `internal/zephyr_os_internal.h`.
- **Apache-2.0** (derived from Zephyr): `shell/zshell.c`, `shell/subcmd.c`,
  `shell/include/subcmd.h`, `compat/shell_engine/shell_argv.c`,
  `compat/port/cpu_load.h`,
  `compat/log/log_ram.c`, `compat/log/log_ring.c`, `internal/log_ring.h`,
  `compat/lib/utils/getopt/getopt_common.h`, the `wifi` command and the
  network management layer under it (`compat/zephyr_wifi_shell.c`,
  `compat/zephyr_wifi_mgmt.c`, `compat/zephyr_wifi_utils.c`,
  `compat/zephyr_net_mgmt.c`, `zephyr/net/net_mgmt.h`,
  `zephyr/net/wifi_mgmt.h`, `zephyr/net/wifi_utils.h`), and the headers `app_log.h`,
  `log_ram.h`, `priorities.h`, `zephyr_compat.h` and `zephyr/` (`device.h`,
  `devicetree.h`, `drivers/hwinfo.h`, `kernel.h`, `shell/shell.h`,
  `spinlock.h`, `sys/atomic.h`, `sys/printk.h`, `sys/sys_getopt.h`,
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
- `compat/zephyr_wifi_mgmt.c` and `zephyr/net/wifi_mgmt.h` have the stored
  credentials' parts (`CONFIG_WIFI_CREDENTIALS`, not defined here);
  `compat/zephyr_wifi_utils.c` has `net_bytes_from_str()`, which nothing
  calls.

## This board's part (`shell/port/`)

| File | The boundary it implements |
|---|---|
| `zephyr_os_port.h` | What `compat/port/threadx/zephyr_kernel_os_impl.h` takes from the ThreadX port and the board: interrupt context, interrupt lockout, busy wait, microsecond clock, assertion |
| `priorities_os.h` | Priority mapping, time slices, the stacks of the shell's thread, the network event thread and `wifi_evt` (`compat/include/priorities.h`) |
| `shell_serial_os_impl.h` | Console transport and stdio redirection (`compat/port/shell_serial_os.h`) |
| `log_ram_os_impl.h` | RAM log: retained section, lock, clock (`compat/port/log_ram_os.h`) |
| `kernel_shell_os_impl.h` | Tick count, restart, download mode, the record for `fault` (`shell/port/kernel_shell_os.h`) |
| `fault_record.h`, `fault_record.c` | The record of a fatal error that `port/panic.c` keeps across the restart (with this board's reasons: abort, failed assertion, failed `ESP_ERROR_CHECK()`), for an Xtensa exception frame: the counterpart of rp2040-connect's `compat/include/fault_record.h` and `compat/log/fault_record.c`, with the same name so that `shell/kernel_shell.c` includes it (`shell/port` is first on the include path); its text for `fault`, the banner and the RAM log, and taking it at the start |
| `shell_port.c` | `hwinfo_get_device_id()`, the layer's tick (`zephyr_compat_tick()`) |
| `zephyr/net/net_if.h`, `zephyr_net_if.c` | The network interface of `<zephyr/net/net_if.h>`: the one interface, its driver's operations, whether it is up and dormant, its IPv4 address, netmask and gateway, link address and MTU, with the functions of rp2040-connect's `compat/net_offload/include/zephyr/net/net_if.h` and `compat/net_offload/zephyr_net_if.c` (there the address type comes from the socket headers, not copied; here the header defines `struct net_in_addr` with the same layout) |
| `zephyr_net_wifi.c`, `zephyr_net_wifi.h` | ESP-IDF's Wi-Fi driver with lwIP (`port/net/wifi.h`) as the offloaded Wi-Fi interface (`struct wifi_mgmt_ops`: scan, connect, disconnect, status), the counterpart of rp2040-connect's `drivers/nina/port/zephyr/nina_net.c`: the interface's address and state on the driver's notifications, and the thread `wifi_evt` that raises the layer's events one at a time, at the priority of the NINA's Wi-Fi thread (`APP_PRIO_NINA_WIFI`); the start-up of the network event thread, the interface and the `wifi` command (`zephyr_net_initialize()`, as rp2040-connect's `compat/net_offload/zephyr_net_init.c`) |
| `zephyr_sys_clock.c` | `sys_clock_gettime()`/`sys_clock_settime()` (`compat/port/generic/zephyr/sys/clock.h`) on the SYSTIMER uptime, as rp2040-connect's `compat/port/pico_sdk/zephyr_sys_clock.c` on the RP2040 TIMER; newlib's `_gettimeofday_r()` and `settimeofday()` on the same clock |
| `commands.c` | `version`, `free`, `ps` (with the run times of ThreadX's execution profile kit) and the command table `shell_engine_cmds[]` |
