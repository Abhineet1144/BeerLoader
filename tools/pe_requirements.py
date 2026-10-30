#!/usr/bin/env python3
"""Inspect PE64 startup requirements using only the Python standard library.

This intentionally reports facts from the guest image; it does not consult a
Windows installation, Wine, Proton, symbol server, or external PE parser.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class PeError(ValueError):
    pass


@dataclass(frozen=True)
class Section:
    name: str
    virtual_size: int
    virtual_address: int
    raw_size: int
    raw_offset: int
    characteristics: int


class Pe64:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        self._require(0, 0x40, "DOS header")
        if self.data[:2] != b"MZ":
            raise PeError("not an MZ executable")
        self.pe_offset = self.u32(0x3C)
        self._require(self.pe_offset, 24, "PE/COFF header")
        if self.data[self.pe_offset : self.pe_offset + 4] != b"PE\0\0":
            raise PeError("missing PE signature")

        coff = self.pe_offset + 4
        (
            self.machine,
            self.section_count,
            self.timestamp,
            _,
            _,
            optional_size,
            self.characteristics,
        ) = self.unpack("<HHIIIHH", coff)
        self.optional_offset = coff + 20
        self._require(self.optional_offset, optional_size, "optional header")
        if self.u16(self.optional_offset) != 0x20B:
            raise PeError("only PE32+ images are supported")
        if optional_size < 112:
            raise PeError("truncated PE32+ optional header")

        opt = self.optional_offset
        self.entry_rva = self.u32(opt + 16)
        self.image_base = self.u64(opt + 24)
        self.section_alignment = self.u32(opt + 32)
        self.file_alignment = self.u32(opt + 36)
        self.os_version = [self.u16(opt + 40), self.u16(opt + 42)]
        self.subsystem_version = [self.u16(opt + 48), self.u16(opt + 50)]
        self.image_size = self.u32(opt + 56)
        self.headers_size = self.u32(opt + 60)
        self.subsystem = self.u16(opt + 68)
        self.dll_characteristics = self.u16(opt + 70)
        directory_count = min(self.u32(opt + 108), 16)
        directory_offset = opt + 112
        if optional_size < 112 + directory_count * 8:
            raise PeError("truncated data-directory table")
        self.directories = [
            {"rva": self.u32(directory_offset + index * 8),
             "size": self.u32(directory_offset + index * 8 + 4)}
            for index in range(directory_count)
        ]
        while len(self.directories) < 16:
            self.directories.append({"rva": 0, "size": 0})

        section_offset = self.optional_offset + optional_size
        self._require(section_offset, self.section_count * 40, "section table")
        self.sections: list[Section] = []
        for index in range(self.section_count):
            offset = section_offset + index * 40
            name = self.data[offset : offset + 8].split(b"\0", 1)[0].decode(
                "ascii", "replace"
            )
            virtual_size, virtual_address, raw_size, raw_offset = self.unpack(
                "<IIII", offset + 8
            )
            characteristics = self.u32(offset + 36)
            self.sections.append(
                Section(name, virtual_size, virtual_address, raw_size,
                        raw_offset, characteristics)
            )

    def _require(self, offset: int, size: int, what: str) -> None:
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise PeError(f"{what} is outside the file")

    def unpack(self, fmt: str, offset: int) -> tuple[Any, ...]:
        size = struct.calcsize(fmt)
        self._require(offset, size, fmt)
        return struct.unpack_from(fmt, self.data, offset)

    def u16(self, offset: int) -> int:
        return self.unpack("<H", offset)[0]

    def u32(self, offset: int) -> int:
        return self.unpack("<I", offset)[0]

    def u64(self, offset: int) -> int:
        return self.unpack("<Q", offset)[0]

    def rva_offset(self, rva: int, size: int = 1) -> int:
        if rva < self.headers_size:
            self._require(rva, size, "header RVA")
            return rva
        for section in self.sections:
            span = max(section.virtual_size, section.raw_size)
            if section.virtual_address <= rva and rva + size <= section.virtual_address + span:
                delta = rva - section.virtual_address
                if delta + size > section.raw_size:
                    raise PeError(f"RVA {rva:#x} has no file-backed data")
                offset = section.raw_offset + delta
                self._require(offset, size, f"RVA {rva:#x}")
                return offset
        raise PeError(f"unmapped RVA {rva:#x}")

    def c_string_rva(self, rva: int) -> str:
        offset = self.rva_offset(rva)
        end = self.data.find(b"\0", offset)
        if end < 0:
            raise PeError(f"unterminated string at RVA {rva:#x}")
        return self.data[offset:end].decode("ascii", "replace")

    def imports(self) -> list[dict[str, Any]]:
        directory = self.directories[1]
        if not directory["rva"]:
            return []
        start = self.rva_offset(directory["rva"], 20)
        modules: list[dict[str, Any]] = []
        for descriptor_index in range(4096):
            offset = start + descriptor_index * 20
            self._require(offset, 20, "import descriptor")
            lookup_rva, timestamp, forwarder, name_rva, iat_rva = self.unpack(
                "<IIIII", offset
            )
            if not any((lookup_rva, timestamp, forwarder, name_rva, iat_rva)):
                return modules
            module_name = self.c_string_rva(name_rva)
            thunk_rva = lookup_rva or iat_rva
            thunk_offset = self.rva_offset(thunk_rva, 8)
            symbols: list[dict[str, Any]] = []
            for thunk_index in range(65536):
                value = self.u64(thunk_offset + thunk_index * 8)
                if value == 0:
                    break
                if value & (1 << 63):
                    symbols.append({"ordinal": value & 0xFFFF})
                else:
                    name_offset = self.rva_offset(value, 3)
                    hint = self.u16(name_offset)
                    end = self.data.find(b"\0", name_offset + 2)
                    if end < 0:
                        raise PeError("unterminated import name")
                    name = self.data[name_offset + 2 : end].decode("ascii", "replace")
                    symbols.append({"name": name, "hint": hint})
            else:
                raise PeError(f"unterminated thunk table for {module_name}")
            modules.append({"module": module_name, "symbols": symbols})
        raise PeError("unterminated import descriptor table")

    def tls(self) -> dict[str, Any] | None:
        directory = self.directories[9]
        if not directory["rva"]:
            return None
        offset = self.rva_offset(directory["rva"], 40)
        start, end, index_address, callbacks_address, zero_fill, characteristics = self.unpack(
            "<QQQQII", offset
        )
        callbacks: list[int] = []
        if callbacks_address:
            callbacks_rva = callbacks_address - self.image_base
            callback_offset = self.rva_offset(callbacks_rva, 8)
            for callback_index in range(4096):
                address = self.u64(callback_offset + callback_index * 8)
                if not address:
                    break
                callbacks.append(address - self.image_base)
            else:
                raise PeError("unterminated TLS callback array")
        return {
            "directory_rva": directory["rva"],
            "directory_size": directory["size"],
            "raw_start_va": start,
            "raw_end_va": end,
            "raw_size": end - start if end >= start else None,
            "index_va": index_address,
            "callbacks_va": callbacks_address,
            "callback_rvas": callbacks,
            "zero_fill": zero_fill,
            "characteristics": characteristics,
        }

    def load_config(self) -> dict[str, Any] | None:
        directory = self.directories[10]
        if not directory["rva"]:
            return None
        offset = self.rva_offset(directory["rva"], min(directory["size"], 4))
        structure_size = self.u32(offset)
        available = min(directory["size"], structure_size)
        result: dict[str, Any] = {
            "directory_rva": directory["rva"],
            "directory_size": directory["size"],
            "structure_size": structure_size,
        }
        fields = {
            "security_cookie_va": (0x58, "Q"),
            "guard_cf_check_va": (0x70, "Q"),
            "guard_cf_dispatch_va": (0x78, "Q"),
            "guard_cf_table_va": (0x80, "Q"),
            "guard_cf_count": (0x88, "Q"),
            "guard_flags": (0x90, "I"),
        }
        for name, (relative, kind) in fields.items():
            size = struct.calcsize("<" + kind)
            if relative + size <= available:
                result[name] = self.unpack("<" + kind, offset + relative)[0]
        return result

    def report(self) -> dict[str, Any]:
        imports = self.imports()
        return {
            "path": str(self.path),
            "file_size": len(self.data),
            "machine": self.machine,
            "pe32_plus": True,
            "image_base": self.image_base,
            "image_size": self.image_size,
            "entry_rva": self.entry_rva,
            "subsystem": self.subsystem,
            "section_alignment": self.section_alignment,
            "file_alignment": self.file_alignment,
            "os_version": self.os_version,
            "subsystem_version": self.subsystem_version,
            "dll_characteristics": self.dll_characteristics,
            "relocations_present": bool(self.directories[5]["rva"]),
            "sections": [section.__dict__ for section in self.sections],
            "exception_directory": self.directories[3],
            "tls": self.tls(),
            "load_config": self.load_config(),
            "import_descriptor_count": len(imports),
            "import_count": sum(len(item["symbols"]) for item in imports),
            "imports": imports,
        }


def hexadecimal(value: int | None) -> str:
    return "-" if value is None else f"0x{value:x}"


def print_text(report: dict[str, Any], include_symbols: bool) -> None:
    print(f"PE: {report['path']}")
    print(f"Machine: {hexadecimal(report['machine'])} (PE32+)")
    print(f"Image base: {hexadecimal(report['image_base'])}")
    print(f"Image size: {hexadecimal(report['image_size'])}")
    print(f"Entry RVA: {hexadecimal(report['entry_rva'])}")
    print(f"Sections: {len(report['sections'])}")
    print(f"Relocations: {'present' if report['relocations_present'] else 'absent'}")
    print(
        f"Imports: {report['import_count']} symbols from "
        f"{report['import_descriptor_count']} descriptors"
    )
    tls = report["tls"]
    if tls:
        callbacks = ", ".join(hexadecimal(value) for value in tls["callback_rvas"])
        print(
            f"TLS: RVA {hexadecimal(tls['directory_rva'])}, raw={tls['raw_size']}, "
            f"zero={tls['zero_fill']}, callbacks=[{callbacks}]"
        )
    else:
        print("TLS: absent")
    load_config = report["load_config"]
    if load_config:
        print(
            f"Load config: RVA {hexadecimal(load_config['directory_rva'])}, "
            f"size={hexadecimal(load_config['directory_size'])}"
        )
    else:
        print("Load config: absent")
    print("\nImport modules:")
    for item in report["imports"]:
        print(f"  {item['module']}: {len(item['symbols'])}")
        if include_symbols:
            for symbol in item["symbols"]:
                label = symbol.get("name", f"ordinal {symbol['ordinal']}")
                print(f"    {label}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="PE32+ executable to inspect")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument(
        "--symbols", action="store_true", help="include each import in text output"
    )
    arguments = parser.parse_args()
    try:
        report = Pe64(arguments.image).report()
    except (OSError, PeError) as error:
        print(f"pe_requirements: {error}", file=sys.stderr)
        return 2
    if arguments.json:
        json.dump(report, sys.stdout, indent=2, sort_keys=True)
        print()
    else:
        print_text(report, arguments.symbols)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
