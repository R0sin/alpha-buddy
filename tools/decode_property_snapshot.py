"""Strict decoder for the data payload of Sony GetAllDevicePropData (0x9209).

Input excludes PTP/IP packet headers. No bytes are skipped to recover alignment.
Descriptor layout follows Alpha-Fairy's PtpIpSonyAlphaCameraPropDecoder.cpp
and libgphoto2 camlibs/ptp2/ptp-pack.c (ptp_unpack_Sony_DPD). Sony's extra
enumeration list is selected explicitly; default is Alpha-Fairy's two lists.
"""

import argparse
import json
from pathlib import Path
import struct


class Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def take(self, size):
        if size < 0 or size > len(self.data) - self.offset:
            raise ValueError(f"truncated at offset {self.offset}: need {size} bytes")
        start = self.offset
        self.offset += size
        return self.data[start:self.offset]

    def uint(self, size):
        return int.from_bytes(self.take(size), "little")

    def value(self, datatype):
        start = self.offset
        if datatype == 0xFFFF:
            raw = self.take(self.uint(1) * 2)
            value = raw.decode("utf-16-le").rstrip("\0")
        else:
            base = datatype & ~0x4000
            if not 1 <= base <= 10 or datatype not in (base, base | 0x4000):
                raise ValueError(f"unsupported datatype 0x{datatype:04X} at {start}")
            size = 1 << ((base - 1) // 2)
            count = self.uint(4) if datatype & 0x4000 else 1
            raw = self.take(count * size)
            values = [int.from_bytes(raw[i:i + size], "little", signed=bool(base & 1))
                      for i in range(0, len(raw), size)]
            value = values if datatype & 0x4000 else values[0]
        return {"value": value, "raw_le": self.data[start:self.offset].hex()}


def decode(data, enum_lists=2):
    if enum_lists not in (1, 2):
        raise ValueError("enum_lists must be 1 or 2")
    reader = Reader(data)
    count = reader.uint(4)
    header_word = reader.uint(4)
    properties = []
    for index in range(count):
        offset = reader.offset
        try:
            code = reader.uint(2)
            datatype = reader.uint(2)
            if code == 0:
                raise ValueError("zero property code")
            get_set, enabled = reader.uint(1), reader.uint(1)
            default = reader.value(datatype)
            current = reader.value(datatype)
            form = reader.uint(1)
            if form == 1:
                for _ in range(3):
                    reader.value(datatype)
            elif form == 2:
                for _ in range(enum_lists):
                    for _ in range(reader.uint(2)):
                        reader.value(datatype)
            elif form != 0:
                raise ValueError(f"unsupported form {form}")
            properties.append({"offset": offset, "code": f"0x{code:04X}",
                               "datatype": f"0x{datatype:04X}", "get_set": get_set,
                               "enabled": enabled, "default": default,
                               "current": current, "form": form})
        except (ValueError, UnicodeError) as error:
            raise ValueError(f"property {index + 1}/{count} at {offset}: {error}") from error
    if reader.offset != len(data):
        raise ValueError(f"{len(data) - reader.offset} trailing bytes at {reader.offset}")
    return {"count": count, "header_word": header_word, "enum_lists": enum_lists,
            "bytes_consumed": reader.offset, "properties": properties}


def self_test():
    # Two integers, including exposure with nonzero high bits; no 16-bit masking.
    exposure = struct.pack("<HHBBIIB", 0x500E, 6, 0, 2, 1, 0x01008050, 2)
    focus = struct.pack("<HHBBHHB", 0x500A, 4, 0, 2, 1, 2, 0)
    for lists in (1, 2):
        data = struct.pack("<II", 2, 0) + exposure
        data += struct.pack("<HI", 1, 0x01008050) * lists + focus
        result = decode(data, lists)
        assert result["properties"][0]["current"] == {"value": 0x01008050, "raw_le": "50800001"}
        assert result["properties"][1]["current"]["value"] == 2
        for bad in (data[:-1], data + b"\0"):
            try:
                decode(bad, lists)
            except ValueError:
                pass
            else:
                raise AssertionError("accepted truncated or trailing data")
    print("self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", nargs="?", type=Path)
    parser.add_argument("--enum-lists", type=int, choices=(1, 2), default=2)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if args.snapshot:
        try:
            result = decode(args.snapshot.read_bytes(), args.enum_lists)
        except (OSError, ValueError) as error:
            parser.exit(1, f"decode failed: {error}\n")
        print(json.dumps(result, indent=2, ensure_ascii=True))
    elif not args.self_test:
        parser.error("provide a snapshot .bin or --self-test")


if __name__ == "__main__":
    main()
