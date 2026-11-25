# Where the shell comes from

`shell/common/` is a copy of part of
[rp2040-connect](https://github.com/owhinata/rp2040-connect) at commit
`72f3485`: the files of its `common/` directory that this firmware compiles or
includes, at the same relative paths.  Nothing else of that repository is
here (no driver, network, Telnet, sensor, Pico SDK or test file).

`shell/port/` is written for this board and is not a copy.

To compare with the original (only the modified files below should differ):

```sh
diff -r shell/common /path/to/rp2040-connect/common | grep -v '^Only in /path/to'
```

## Copied files

All 60 files are byte-identical to the original, except the ones below:

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
  `shell/dmesg_shell.c`, and the headers `fault_record.h`, `shell_console.h`,
  `shell_engine.h`, `shell_serial.h`, `zephyr_compat_os.h`, `zephyr_heap.h`,
  `zephyr/logging/log.h`, `zephyr/toolchain.h`, `internal/cli.h`,
  `internal/shell_argv.h`, `internal/zephyr_os_internal.h`.
- **Apache-2.0** (derived from Zephyr): `shell/zshell.c`, `shell/subcmd.c`,
  `shell/include/subcmd.h`, `compat/shell_engine/shell_argv.c`,
  `compat/log/log_ram.c`, `compat/log/log_ring.c`, `internal/log_ring.h`,
  `compat/lib/utils/getopt/getopt_common.h`, and the headers `app_log.h`,
  `log_ram.h`, `priorities.h`, `zephyr_compat.h` and `zephyr/` (`device.h`,
  `devicetree.h`, `drivers/hwinfo.h`, `kernel.h`, `shell/shell.h`,
  `spinlock.h`, `sys/atomic.h`, `sys/printk.h`, `sys/sys_getopt.h`,
  `sys/util.h`).
- **BSD-3-Clause** (through Zephyr, from NetBSD and OpenBSD):
  `compat/lib/utils/getopt/getopt.c`, `getopt_long.c`,
  `compat/subsys/portability/posix/c_lib_ext/fnmatch.c`,
  `compat/include/zephyr/posix/fnmatch.h`.

Three of the copied files are compiled or included for a part only, and the
rest of each is dropped at link time or never expanded:

- `shell/kernel_shell.c` also holds `date` and `fault`, which the command
  table (`shell/port/commands.c`) does not list.
- `shell/zshell.c` holds Zephyr's device-name helpers, which no command here
  calls.
- `compat/include/fault_record.h` is included by `kernel_shell.c`; its
  functions (`compat/log/fault_record.c`) are not built.

## This board's part (`shell/port/`)

| File | The boundary it implements |
|---|---|
| `zephyr_os_port.h` | What `compat/port/threadx/zephyr_kernel_os_impl.h` takes from the ThreadX port and the board: interrupt context, interrupt lockout, busy wait, microsecond clock, assertion |
| `priorities_os.h` | Priority mapping, time slices, the shell thread's stack (`compat/include/priorities.h`) |
| `shell_serial_os_impl.h` | Console transport and stdio redirection (`compat/port/shell_serial_os.h`) |
| `log_ram_os_impl.h` | RAM log: retained section, lock, clock (`compat/port/log_ram_os.h`) |
| `kernel_shell_os_impl.h` | Tick count, restart, download mode (`shell/port/kernel_shell_os.h`) |
| `shell_port.c` | `hwinfo_get_device_id()`, the layer's tick (`zephyr_compat_tick()`) |
| `commands.c` | `version`, `free`, `ps` and the command table `shell_engine_cmds[]` |
