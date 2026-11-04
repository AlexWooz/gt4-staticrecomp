"""Rewrites the PRIM field of GIF tags in a .gscap (tags with PRE = 1 and PRIM == <from>) to <to>.

usage: patch_prim.py <in .gscap> <out .gscap> <from hex> <to hex>
Walks every GIF record's qwords as a GIF tag stream (PACKED / REGLIST / IMAGE sizes) to find the tags.
"""
import struct
import sys

src, dst, frm, to = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
data = bytearray(open(src, 'rb').read())
pos = 64
patched = 0
pending = {}  # path -> qwords still to skip from a tag that continues in the next record
while pos + 16 <= len(data):
    rtype, flags, nbytes, seq = struct.unpack_from('<HHIQ', data, pos)
    body = pos + 16
    if rtype == 1:
        path = data[body]
        gbytes = struct.unpack_from('<I', data, body + 8)[0]
        q = body + 16
        end = q + gbytes
        skip = pending.get(path, 0)
        while q + 16 <= end:
            if skip > 0:
                step = min(skip, (end - q) // 16)
                q += 16 * step
                skip -= step
                continue
            lo, hi = struct.unpack_from('<QQ', data, q)
            nloop = lo & 0x7FFF
            pre = (lo >> 46) & 1
            prim = (lo >> 47) & 0x7FF
            flg = (lo >> 58) & 3
            nreg = (lo >> 60) & 0xF or 16
            if pre and prim == frm:
                lo = (lo & ~(0x7FF << 47)) | (to << 47)
                struct.pack_into('<Q', data, q, lo)
                patched += 1
            q += 16
            if flg == 0:
                skip = nloop * nreg
            elif flg == 1:
                skip = (nloop * nreg + 1) // 2
            else:
                skip = nloop
        pending[path] = skip
    pos = body + ((nbytes + 15) & ~15)
open(dst, 'wb').write(data)
print('patched tags', patched)
