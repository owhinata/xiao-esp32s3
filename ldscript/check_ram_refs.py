#!/usr/bin/env python3
"""
Post-link check of the code and data that the linker script keeps in RAM
because they must work without the flash (ldscript/xiao_esp32s3.ld): none
of it may refer to the flash, nor to the code in RAM for speed only.

That code runs from RAM because it has to work while the cache is off
(the flash driver's windows, port/board_flash.c), before the flash mapping
is set up, or while it may be broken (the fault path).  A reference from
there into the flash is a call, a jump target or a data address that the
CPU fetches through the cache; with the cache off it stalls or faults.  The
code in .iram0.fast is in RAM only for speed and calls into the flash
freely, so a reference to it counts as one into the flash.  This check
finds those references in the linked image.

Checked, from the linker map:
  - every input section of .iram0.vectors (the exception and interrupt
    vectors);
  - in .iram0.text and .dram0.data, every input section (code, literals,
    read-only data, data, DRAM_ATTR data) of the objects that the linker
    script's RAM lists name explicitly (a pattern whose file part is not
    just "*"), and of the objects in EXTRA_OBJECTS below.
What lands in RAM only through the generic patterns (Espressif's
IRAM_ATTR and DRAM_ATTR in objects that otherwise stay in flash, the Wi-Fi
and PHY libraries' .iram1), and .iram0.fast, are not checked: none of it
runs while the cache is off here (every interrupt up to XCHAL_EXCM_LEVEL is
masked then, and no thread switch happens) or on the fault path.  --all
lists their references too.
The image is linked with --emit-relocs, so every word and instruction that
holds an address is known from its relocation: a literal (R_XTENSA_32, the
word's final value) or an instruction operand (R_XTENSA_SLOTn_OP: for a
call or a jump the target its encoding gives, else the symbol's address
plus the addend, which for a section symbol is not adjusted to the output
section).  Plain constants that only look like flash addresses have no
relocation and are not reported.  A reference counts as one into the flash
when it lies in 0x42000000-0x44000000 (code), 0x3C000000-0x3E000000 (data)
or .iram0.fast.

ALLOWED below lists the references that are known to be harmless, each
with its reason.  Any other reference fails the build (exit status 1) with
the place (object, input section, function) and the target.

Usage: check_ram_refs.py <image.elf> <image.map> [--all]
  --all also lists the allowed references.
Only the Python standard library is used.
"""

import re
import struct
import sys

FLASH_RANGES = ((0x42000000, 0x44000000, "code"), (0x3C000000, 0x3E000000, "data"))

R_XTENSA_32 = 1
R_XTENSA_SLOT0_OP = 20
R_XTENSA_SLOT14_OP = 34

# Objects that the vectors reach and that are in RAM through the generic
# .iram1 pattern only: the level-4 handler (the interrupt watchdog and the
# cache error interrupt, both open while the cache is off) and _xt_panic,
# the entry of the fault path for exceptions.
EXTRA_OBJECTS = (
    "highint_hdl.S.obj",
    "panic_handler_asm.S.obj",
)

# (object file name, input section pattern, target symbol, reason).  The
# object is the file name of the object (of the member, in an archive), the
# section and the symbol are regular expressions on the whole name.
ALLOWED = (
    ("startup.c.obj", r"\.literal\.cache_init", r"_flash_text_(start|end)|_instruction_reserved_(start|end)",
     "the bounds of .flash.text as numbers (its size, for the MMU split; "
     "_instruction_reserved_* are other names of the same addresses), not "
     "read"),
    ("startup.c.obj", r"\.entry\.literal", r".*",
     "_start calls into the flash only after cache_init(), its own function, "
     "has set the cache up (port/startup.c)"),
    ("startup.c.obj", r"\.literal\.board_sys_.*", r".*",
     "the start-up hooks of the later stages, run from threads and "
     "tx_application_define() with the cache on"),
    ("panic.c.obj", r"\.literal\.(fault_seal|fault_dump_and_restart|"
     r"xt_unhandled_exception|panicHandler)", r".*",
     "after the first seal of the record, once board_flash_fault() has "
     "resumed the cache (port/panic.c)"),
    ("memspi_host_driver.c.obj", r"\.literal\.memspi_host_init_spi1_pointers",
     r"spi_flash_hal_init",
     "the start-up of the chip (esp_flash_init_default_chip()), with the "
     "cache on; spi_flash_hal.c stays in flash as in ESP-IDF "
     "(esp_hal_mspi/linker.lf)"),
    ("memspi_host_driver.c.obj", r"\.dram1\..*", r"spi_flash_hal_supports_direct_(read|write)",
     "the host driver's table: called by esp_flash_read() and "
     "esp_flash_write() before start(), with the cache on; in flash as in "
     "ESP-IDF"),
    ("tx_timer_interrupt.S.obj", r"\.literal", r".*",
     "the tick's C part: an interrupt handler in flash, masked while the "
     "cache is off (port/board_flash_os.c)"),
    ("tx_timer_interrupt.S.obj", r"\.text", r"_tx_timer_expiration_process|_tx_thread_time_slice",
     "the same, for the functions the tick calls directly"),
    ("rtc_init.c.obj", r"\.literal\.rtc_init", r"rtc_sleep_pu|sar_periph_ctrl_init",
     "called by rtc_init() at start-up with the cache on, not during the "
     "o-code calibration"),
    ("rtc_time.c.obj", r"\.literal\..*enable_timer_group0_for_calibration",
     r"periph_rcc_acquire_(enter|exit)",
     "the clock calibration, at start-up and from threads, with the cache on"),
    ("rtc_clk.c.obj", r"\.literal\.rtc_clk_slow_src_set", r"esp_sleep_sub_mode_.*",
     "the RTC slow clock's selection, at start-up with the cache on"),
    ("esp_cache_msync.c.obj", r"\.literal\.esp_cache_sync_ops_exit_critical_section",
     r"_tx_thread_system_preempt_check",
     "the thread switch at the end of a critical section, made only when "
     "the PS it restores has the interrupts open; it is entered only with "
     "them masked (the cache freeze, port/esp_compat/esp_cache_utils.c, and "
     "the flash driver's windows)"),
    ("esp_mmu_map.c.obj", r"\.dram1\..*", r".*",
     "the MMU driver's log tag (a DRAM_ATTR pointer to its string in "
     "flash), read only by its functions in flash: the ones in RAM log "
     "nothing at this log level"),
)

CODE_SECTIONS = (".iram0.vectors", ".iram0.text")
DATA_SECTION = ".dram0.data"
FAST_SECTION = ".iram0.fast"


def slot_target(elf, sec, addr, sym, addend):
    """The address an instruction operand refers to (see the top)."""
    insn = int.from_bytes(elf.data[sec["off"] + addr - sec["addr"]:][:3], "little")
    op0 = insn & 0xF
    if op0 == 5 or (op0 == 6 and (insn >> 4) & 3 == 0):     # CALLn, J
        offset = (insn >> 6) & 0x3FFFF
        if offset & 0x20000:
            offset -= 0x40000
        if op0 == 5:
            return ((addr & ~3) + (offset << 2) + 4) & 0xFFFFFFFF
        return (addr + offset + 4) & 0xFFFFFFFF
    return (sym["value"] + addend) & 0xFFFFFFFF


class Elf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise SystemExit(f"{path}: not a 32-bit little-endian ELF file")
        (shoff,) = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
        self.sections = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                "<10I", d, shoff + i * shentsize)
            self.sections.append(dict(name_off=name, type=typ, addr=addr, off=off, size=size,
                                      link=link, info=info, entsize=entsize))
        strtab = self.sections[shstrndx]
        for s in self.sections:
            s["name"] = self._str(strtab, s["name_off"])
        self.by_name = {s["name"]: s for s in self.sections}
        symtab = self.by_name.get(".symtab")
        if symtab is None:
            raise SystemExit(f"{path}: no symbol table")
        names = self.sections[symtab["link"]]
        self.symbols = []
        for i in range(symtab["size"] // 16):
            name, value, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", d, symtab["off"] + i * 16)
            self.symbols.append(dict(name=self._str(names, name), value=value, size=size,
                                     type=info & 0xF, shndx=shndx))
        # Functions, objects and untyped symbols (the linker script's), defined.
        named = [s for s in self.symbols if s["name"] and s["type"] in (0, 1, 2)
                 and s["shndx"] != 0 and not s["name"].startswith(".L")]
        named.sort(key=lambda s: (s["value"], -s["size"]))
        self.named = named

    def _str(self, sec, off):
        start = sec["off"] + off
        end = self.data.index(b"\0", start)
        return self.data[start:end].decode("ascii", "replace")

    def word(self, sec, addr):
        return struct.unpack_from("<I", self.data, sec["off"] + addr - sec["addr"])[0]

    def relocations(self, target):
        """(address, type, symbol index, addend) of the relocations of target."""
        index = self.sections.index(target)
        for s in self.sections:
            if s["type"] == 4 and s["info"] == index:       # SHT_RELA
                for i in range(s["size"] // 12):
                    off, info, addend = struct.unpack_from("<IIi", self.data, s["off"] + i * 12)
                    yield off, info & 0xFF, info >> 8, addend

    def name_of(self, addr):
        """The symbol at addr, else the one that addr lies in, else the nearest one below it."""
        best = None
        lo, hi = 0, len(self.named)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.named[mid]["value"] <= addr:
                lo = mid + 1
            else:
                hi = mid
        for s in reversed(self.named[max(0, lo - 8):lo]):
            if s["value"] == addr:
                return s["name"]
        for s in reversed(self.named[max(0, lo - 8):lo]):
            if s["value"] <= addr < s["value"] + max(s["size"], 1):
                return s["name"] if s["value"] == addr else f"{s['name']}+0x{addr - s['value']:x}"
            if best is None:
                best = s
        if best is None:
            return "?"
        return f"{best['name']}+0x{addr - best['value']:x}"


def parse_map(path, wanted):
    """
    Input sections (start, size, name, object, file part of the pattern that
    placed it) of the output sections in wanted.
    """
    out = {w: [] for w in wanted}
    current = None
    pending = None
    pattern = None
    head = re.compile(r"^(\.\S+)\s+0x[0-9a-f]+\s+0x[0-9a-f]+")
    line_pattern = re.compile(r"^ (?:EXCLUDE_FILE\([^)]*\) )?(\*[^ (]*)\(.*\)\s*$")
    line_full = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
    line_name = re.compile(r"^ (\S+)\s*$")
    line_rest = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = head.match(line)
            if m:
                current = m.group(1) if m.group(1) in out else None
                pending = None
                pattern = None
                continue
            if current is None:
                continue
            m = line_pattern.match(line)
            if m and not line.startswith(" *fill*"):
                pattern = m.group(1)
                pending = None
                continue
            m = line_full.match(line)
            if m and not m.group(1).startswith("*"):
                out[current].append((int(m.group(2), 16), int(m.group(3), 16), m.group(1),
                                     m.group(4), pattern))
                pending = None
                continue
            m = line_name.match(line)
            if m and not m.group(1).startswith("*"):
                pending = m.group(1)
                continue
            m = line_rest.match(line)
            if m and pending is not None:
                out[current].append((int(m.group(1), 16), int(m.group(2), 16), pending,
                                     m.group(3), pattern))
            pending = None
    for w in out:
        out[w].sort()
    return out


def input_section_at(sections, addr):
    lo, hi = 0, len(sections)
    while lo < hi:
        mid = (lo + hi) // 2
        if sections[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    for start, size, name, obj, _ in reversed(sections[max(0, lo - 4):lo]):
        if start <= addr < start + size:
            return name, obj
    return None


def short_object(obj):
    obj = obj.replace("\\", "/")
    m = re.match(r"^(.*/)?([^/]+\.a)\((.*)\)$", obj)
    if m:
        return f"{m.group(2)}({m.group(3)})"
    return obj.split("/")[-1]


def object_name(obj):
    """The object's file name: the member of an archive, else the file's."""
    m = re.match(r"^.*\((.*)\)$", obj)
    return m.group(1) if m else obj.replace("\\", "/").split("/")[-1]


def allowed(obj, section, target):
    for a_obj, a_sec, a_sym, _ in ALLOWED:
        if (object_name(obj) == a_obj and re.fullmatch(a_sec, section)
                and re.fullmatch(a_sym, target.split("+")[0])):
            return True
    return False


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    elf = Elf(argv[1])
    show_all = "--all" in argv[3:]
    maps = parse_map(argv[2], CODE_SECTIONS + (DATA_SECTION,))
    listed = {obj for out_name in (".iram0.text", DATA_SECTION) for _, _, _, obj, pat in maps[out_name]
              if pat is not None and pat != "*"}
    listed |= {obj for out_name in CODE_SECTIONS + (DATA_SECTION,) for _, _, _, obj, _ in maps[out_name]
               if object_name(obj) in EXTRA_OBJECTS}
    fast = elf.by_name.get(FAST_SECTION)
    ranges = FLASH_RANGES
    if fast is not None and fast["size"]:
        ranges += ((fast["addr"], fast["addr"] + fast["size"], "fast code"),)
    found = []
    for out_name in CODE_SECTIONS + (DATA_SECTION,):
        sec = elf.by_name.get(out_name)
        if sec is None:
            continue
        if not any(True for _ in elf.relocations(sec)):
            raise SystemExit(f"{argv[1]}: no relocations for {out_name}; link with --emit-relocs")
        for addr, typ, symi, addend in elf.relocations(sec):
            where = input_section_at(maps[out_name], addr)
            if where is None:
                continue
            in_name, obj = where
            checked = out_name == ".iram0.vectors" or obj in listed
            if typ == R_XTENSA_32:
                target = elf.word(sec, addr)
            elif R_XTENSA_SLOT0_OP <= typ <= R_XTENSA_SLOT14_OP:
                sym = elf.symbols[symi]
                if sym["shndx"] == 0:
                    continue
                target = slot_target(elf, sec, addr, sym, addend)
            else:
                continue
            kind = next((k for lo, hi, k in ranges if lo <= target < hi), None)
            if kind is None:
                continue
            target_name = elf.name_of(target)
            status = ("allowed" if allowed(obj, in_name, target_name) else "bad") if checked \
                else "unchecked"
            found.append((obj, in_name, addr, target, target_name, kind, status))
    bad = [f for f in found if f[6] == "bad"]
    for obj, in_name, addr, target, target_name, kind, status in found:
        if status != "bad" and not show_all:
            continue
        print(f"{status + ': ' if status != 'bad' else ''}{short_object(obj)}({in_name}) "
              f"at 0x{addr:08x}: {kind} 0x{target:08x} <{target_name}>",
              file=sys.stderr if status == "bad" else sys.stdout)
    if bad:
        print(f"check_ram_refs: {len(bad)} reference(s) from RAM into the flash or "
              f"{FAST_SECTION} (ldscript/check_ram_refs.py)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
