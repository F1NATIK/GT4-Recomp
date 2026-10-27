#!/usr/bin/env python3
"""Convert GT4 USA CORE.GT4 (SCUS-97328) to a byte-preserving MIPS ELF.

Adapted from GT3RECOMP/gt3-tools/core_to_elf.py (same container: Nenkai/PDTools
GT4ElfBuilderTool). No game instruction bytes are patched.

Layout read from this exact image (2026-10-03):
  segment 0x6179fc..0x617a14  .reginfo (gp 0x6dddf0), the tail of segment 1
  segment 0x100000..0x617a14  code; the last function returns at 0x616f14,
                              then the constructor / destructor tables at 0x616f20
  segment 0x617a80..0x6d5dfc  data
  startup clears BSS 0x6d5e00..0x8a215c (0x10011c..0x100194) and loads gp 0x6dddf0
The RSA signature is NOT verified: GT3's exponent (66001) and the odd constants
stored in SCUS_973.28 do not validate it. The input is pinned by SHA-256 instead.
"""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

CORE_SHA256 = '85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9'
TEXT_END = 0x616f20
BSS_START, BSS_END, GP = 0x6d5e00, 0x8a215c, 0x6dddf0
# EE code stored inside .data (rt_crypt SHA-1/MD5, called by jal from .text; phase 2
# recomp-prep finding 14). Given its own executable section so ps2_recomp translates it at
# its real address. Bytes and program headers are unchanged; only section headers differ.
DATA_CODE = (0x6c5400, 0x6c833c)


def convert(source, target):
    packed = source.read_bytes()
    flags, expected = struct.unpack_from('<HI', packed)
    if flags != 0x0101 or not 0 < expected <= 32 * 1024 * 1024:
        raise ValueError('Unsupported CORE header')
    inflater = zlib.decompressobj(-15)
    raw = inflater.decompress(packed[6:], expected + 1)
    if len(raw) != expected or not inflater.eof or inflater.unconsumed_tail:
        raise ValueError('CORE decompression length or end-of-stream mismatch')
    pos = 0
    for _ in range(2):
        size = struct.unpack_from('<H', raw, pos)[0]
        if size != 128:
            raise ValueError('Unexpected RSA field')
        pos += 2 + size
    count, entry = struct.unpack_from('<II', raw, pos)
    pos += 8
    segments = []
    for _ in range(count):
        base, size = struct.unpack_from('<II', raw, pos)
        pos += 8
        segments.append((base, raw[pos:pos + size]))
        pos += size
    if pos != len(raw) or entry != 0x100008 or count != 3:
        raise ValueError('Unexpected GT4 CORE segment layout')

    lo = min(b for b, _ in segments)
    hi = max(b + len(p) for b, p in segments)
    image = bytearray(hi - lo)
    for base, payload in segments:
        image[base - lo:base - lo + len(payload)] = payload
    if lo != 0x100000 or hi > BSS_START:
        raise ValueError('Initialized image bounds unexpected')
    reginfo = dict((b, p) for b, p in segments)[0x6179fc]
    if struct.unpack_from('<I', reginfo, 20)[0] != GP:
        raise ValueError('reginfo gp mismatch')
    # Check the startup code still matches the BSS / GP constants used here.
    def word(addr):
        return struct.unpack_from('<I', image, addr - lo)[0]
    startup = {0x10011c: 0x3c02006d, 0x100120: 0x3c03008a, 0x100124: 0x24425e00,
               0x100128: 0x2463215c, 0x100198: 0x3c04006e, 0x1001ac: 0x2484ddf0}
    if any(word(a) != op for a, op in startup.items()):
        raise ValueError('Unrecognized startup layout: inspect BSS and GP manually')
    if word(TEXT_END - 0xc) != 0x03e00008 or word(TEXT_END + 4) != 0xffffffff:
        raise ValueError('Code/data boundary marker not found: review TEXT_END')
    sha = hashlib.sha256(packed).hexdigest()
    if CORE_SHA256 and sha != CORE_SHA256:
        raise ValueError('Unknown CORE build: code/data boundary needs review')

    size = len(image)
    memory_end = (BSS_END + 31) & ~15  # startup SQ loop also writes the last block
    text_size = TEXT_END - lo
    names = b'\0.text\0.data\0.bss\0.reginfo\0.shstrtab\0.text.rt_crypt\0.data.1\0'
    name_at = {n: names.index(b'\0' + n.encode() + b'\0') + 1 for n in
               ('.text', '.data', '.bss', '.reginfo', '.shstrtab', '.text.rt_crypt', '.data.1')}
    section_count = 8
    data_offset = 0x1000
    reg_offset = (data_offset + size + 3) & ~3
    names_offset = reg_offset + 24
    sh_offset = (names_offset + len(names) + 3) & ~3
    elf = bytearray(sh_offset + section_count * 40)
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<16sHHIIIIIHHHHHH', elf, 0, ident, 2, 8, 1, entry,
                     52, sh_offset, 0x20924001, 52, 32, 2, 40, section_count, section_count - 1)
    struct.pack_into('<8I', elf, 52, 1, data_offset, lo, lo, text_size,
                     text_size, 5, 0x1000)
    struct.pack_into('<8I', elf, 84, 1, data_offset + text_size, lo + text_size,
                     lo + text_size, size - text_size, memory_end - lo - text_size, 6, 0x1000)
    elf[data_offset:data_offset + size] = image
    struct.pack_into('<6I', elf, reg_offset, 0, 0, 0, 0, 0, GP)
    elf[names_offset:names_offset + len(names)] = names
    def off(addr):
        return data_offset + addr - lo
    code_lo, code_hi = DATA_CODE
    sections = [
        (name_at['.text'], 1, 6, lo, data_offset, text_size, 0, 0, 64, 0),
        (name_at['.data'], 1, 3, TEXT_END, off(TEXT_END), code_lo - TEXT_END, 0, 0, 64, 0),
        (name_at['.text.rt_crypt'], 1, 6, code_lo, off(code_lo), code_hi - code_lo, 0, 0, 4, 0),
        (name_at['.data.1'], 1, 3, code_hi, off(code_hi), lo + size - code_hi, 0, 0, 4, 0),
        (name_at['.bss'], 8, 3, BSS_START, data_offset + size, memory_end - BSS_START, 0, 0, 16, 0),
        (name_at['.reginfo'], 0x70000006, 0, 0, reg_offset, 24, 0, 0, 4, 24),
        (name_at['.shstrtab'], 3, 0, 0, names_offset, len(names), 0, 0, 1, 0),
    ]
    for i, section in enumerate(sections, 1):
        struct.pack_into('<10I', elf, sh_offset + i * 40, *section)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(elf)
    report = {
        'source_sha256': sha,
        'elf_sha256': hashlib.sha256(elf).hexdigest(),
        'core_rsa_verified': False,
        'decompressed_bytes': len(raw),
        'segments': [[hex(b), hex(len(p))] for b, p in segments],
        'entry': hex(entry), 'gp': hex(GP),
        'text': [hex(lo), hex(TEXT_END)], 'data': [hex(TEXT_END), hex(hi)],
        'bss_start': hex(BSS_START), 'bss_logical_end': hex(BSS_END),
        'memory_end_including_startup_padding': hex(memory_end),
        'instruction_bytes_changed': 0,
        'data_code_section': [hex(DATA_CODE[0]), hex(DATA_CODE[1])],
        'section_note': 'Text ends after the last function (jr ra at 0x616f14); 0x616f20 starts the ctor/dtor tables (0, -1, pointers). Gap 0x617a14..0x617a80 is zero-filled.'
    }
    target.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('core', type=Path)
    parser.add_argument('elf', type=Path)
    args = parser.parse_args()
    print(json.dumps(convert(args.core, args.elf), indent=2))
