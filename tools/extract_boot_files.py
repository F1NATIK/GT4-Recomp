#!/usr/bin/env python3
"""Copy GT4's boot files (everything outside GT4.VOL and the EPSON printer
drivers) from the ISO into game/, read only on the ISO. Writes a SHA-256 manifest.
Usage: extract_boot_files.py <iso> <outdir>"""
import hashlib, json, struct, sys
from pathlib import Path

iso, out = Path(sys.argv[1]), Path(sys.argv[2])
f = iso.open('rb')
def sec(n, c=1):
    f.seek(n * 2048); return f.read(2048 * c)
def entries(lba, size):
    data = sec(lba, (size + 2047) // 2048); i = 0
    while i < size:
        n = data[i]
        if n == 0:
            i = (i // 2048 + 1) * 2048; continue
        rec = data[i:i + n]; i += n
        name = rec[33:33 + rec[32]]
        if name in (b'\0', b'\1'): continue
        yield name.decode().split(';')[0], *struct.unpack_from('<I', rec, 2), *struct.unpack_from('<I', rec, 10), bool(rec[25] & 2)
pvd = sec(16)
manifest = {}
def walk(lba, size, prefix):
    for name, elba, esize, isdir in entries(lba, size):
        rel = prefix + name
        if isdir:
            if name not in ('EPSON',): walk(elba, esize, rel + '/')
            continue
        if name == 'GT4.VOL': 
            manifest[rel] = {'lba': elba, 'size': esize, 'extracted': False}; continue
        f.seek(elba * 2048); data = f.read(esize)
        p = out / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data)
        manifest[rel] = {'lba': elba, 'size': esize, 'sha256': hashlib.sha256(data).hexdigest()}
walk(*struct.unpack_from('<I', pvd, 156 + 2), *struct.unpack_from('<I', pvd, 156 + 10), '')
manifest['_volume'] = {'id': pvd[40:72].decode().strip(), 'iso9660_blocks': struct.unpack_from('<I', pvd, 80)[0], 'iso_bytes': iso.stat().st_size}
(out / 'boot-files-manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
print(json.dumps(manifest['_volume']), len(manifest) - 1, 'files')
