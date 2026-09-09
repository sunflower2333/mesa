#!/usr/bin/env python3
"""Check ARM64EC linked-image identity, not runtime compatibility."""

import pathlib
import struct
import sys


def verify(path):
    data = pathlib.Path(path).read_bytes()

    def get(fmt, offset):
        return struct.unpack_from("<" + fmt, data, offset)[0]

    if data[:2] != b"MZ":
        raise ValueError("missing DOS header")
    pe = get("I", 0x3c)
    if data[pe:pe + 4] != b"PE\0\0" or get("H", pe + 4) != 0x8664:
        raise ValueError("ARM64EC linked images must use the AMD64 PE machine")
    optional = pe + 24
    if get("H", optional) != 0x20b or get("H", pe + 20) < 208:
        raise ValueError("missing PE32+ optional header")
    if get("I", optional + 108) <= 10:
        raise ValueError("missing load-config directory")
    sections = optional + get("H", pe + 20)

    def file_offset(rva, length):
        for index in range(get("H", pe + 6)):
            section = sections + 40 * index
            address = get("I", section + 12)
            raw_size = get("I", section + 16)
            raw = get("I", section + 20)
            delta = rva - address
            if 0 <= delta and delta + length <= raw_size and raw + delta + length <= len(data):
                return raw + delta
        raise ValueError("metadata RVA has no complete file backing")

    config_rva = get("I", optional + 112 + 10 * 8)
    config_size = get("I", optional + 112 + 10 * 8 + 4)
    if not config_rva or config_size < 208:
        raise ValueError("load config has no CHPE metadata pointer")
    config = file_offset(config_rva, 208)
    if get("I", config) < 208:
        raise ValueError("load-config structure is too short")
    metadata_va = get("Q", config + 200)
    image_base = get("Q", optional + 24)
    if metadata_va < image_base:
        raise ValueError("missing ARM64EC hybrid metadata")
    metadata = file_offset(metadata_va - image_base, 12)
    version, code_map, code_count = struct.unpack_from("<III", data, metadata)
    if not version or not code_count:
        raise ValueError("hybrid metadata has no code ranges")
    file_offset(code_map, code_count * 8)
    print(f"ARM64EC PE identity: machine=0x8664 hybrid-version={version} code-ranges={code_count}")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise ValueError("usage: verify-arm64ec-pe.py DLL")
        verify(sys.argv[1])
    except (OSError, ValueError, struct.error) as error:
        sys.exit(str(error))
