"""Synthetic GS stream (.gscap v2) for black-box rasterisation tests: gt4-gs.exe and our replay draw the same records.

usage: gen_gscap.py <display source .gscap> <out .gscap>
The display registers (SMODE, PMODE, DISPFB, DISPLAY, ...) are copied from the first frame of a real capture, then
the test redirects DISPFB1 to a CT32 buffer at block 0 (FBW 10) and draws:
  - a dark full-screen sprite;
  - white bands made of two triangles each, separated by thin gaps of known height at known sub-pixel positions;
  - a grey strip whose right edge sits at a known fractional X.
Every frame repeats the drawing, then a VSYNC, for 8 frames.
"""
import struct
import sys

src, out = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read(64 << 20)
header = bytearray(data[:64])
pos = 64
privs = []
while pos + 16 <= len(data):
    rtype, flags, nbytes, seq = struct.unpack_from('<HHIQ', data, pos)
    payload = data[pos + 16:pos + 16 + nbytes]
    pos += 16 + ((nbytes + 15) & ~15)
    if rtype == 2:
        address, _, value = struct.unpack('<IIQ', payload[:16])
        privs = [(a, v) for a, v in privs if a != address] + [(address, value)]
    if rtype == 3:
        vsyncs = locals().get('vsyncs', 0) + 1
        if vsyncs >= 4:
            break
print('display registers copied:', [(hex(a), hex(v)) for a, v in privs])

records = []
seq = [0]


def record(rtype, payload):
    seq[0] += 1
    pad = (-len(payload)) % 16
    records.append(struct.pack('<HHIQ', rtype, 0, len(payload), seq[0]) + payload + b'\0' * pad)


def priv(address, value):
    record(2, struct.pack('<IIQ', address, 0, value))


def gif(qwords):
    body = b''.join(qwords)
    record(1, struct.pack('<BBBBIII', 3, 0, 0, 0, 0, len(body), 0) + body)


def tag_ad(count, eop=1):
    # PACKED, NREG 1, REGS = A+D (0xE), NLOOP = count.
    lo = count | (eop << 15) | (0 << 46) | (0 << 47) | (0 << 58) | (1 << 60)
    return struct.pack('<QQ', lo, 0xE)


def ad(register, value):
    return struct.pack('<QQ', value, register)


def xyz2(x, y, z=0):
    return x | (y << 16) | (z << 32)


def rgbaq(r, g, b, a=128):
    return r | (g << 8) | (b << 16) | (a << 24) | (0x3F800000 << 32)


def fx(v):  # 12.4 fixed point, window coordinates (XYOFFSET = 0)
    return int(round(v * 16))


def band(x0, y0, x1, y1, colour):
    """A rectangle as two triangles (shared diagonal, exact shared vertices)."""
    regs = [ad(0x00, 3), ad(0x01, rgbaq(*colour))]
    for tri in (((x0, y0), (x1, y0), (x0, y1)), ((x1, y0), (x0, y1), (x1, y1))):
        for x, y in tri:
            regs.append(ad(0x05, xyz2(fx(x), fx(y))))
    return regs


for frame in range(8):
    for address, value in privs:
        priv(address, value)
    # SMODE1 is set before the capture starts: CMOD 0, LC 32 is the 480p mode GT4_FORCE_480P selects.
    priv(0x12000010, 32 << 3)
    # DISPFB2 (the circuit GT4's races use): FBP 0, FBW 10, CT32 at (0, 0).
    priv(0x12000090, 0 | (10 << 9) | (0 << 15))
    regs = [
        ad(0x4C, 0 | (10 << 16) | (0 << 24)),            # FRAME_1: FBP 0, FBW 10, CT32
        ad(0x4E, (140) | (1 << 24) | (1 << 32)),         # ZBUF_1: ZBP page 140, Z24, ZMSK
        ad(0x47, (1 << 16) | (1 << 17)),                 # TEST_1: ZTE, ZTST ALWAYS
        ad(0x18, 0),                                     # XYOFFSET_1
        ad(0x40, 0 | (639 << 16) | (0 << 32) | (447 << 48)),  # SCISSOR_1
        ad(0x1A, 1),                                     # PRMODECONT
        ad(0x00, 6), ad(0x01, rgbaq(30, 30, 30)), ad(0x05, xyz2(fx(0), fx(0))), ad(0x05, xyz2(fx(640), fx(448))),
    ]
    # Gap tests: white bands above and below a dark gap of height h starting at y0 + frac.
    y = 40.0
    for h, frac in ((0.5, 0.0625), (0.5, 0.3125), (0.5, 0.5625), (0.25, 0.0625), (0.25, 0.4375), (0.125, 0.0625),
                    (0.0625, 0.0), (0.0625, 0.5)):
        top = y + frac
        regs += band(100, y - 8, 300, top, (255, 255, 255))
        regs += band(100, top + h, 300, top + h + 8, (255, 255, 255))
        y += 24
    # Edge tests: grey strips whose right edge is at x = 400 + k/16, one per row.
    for k in range(16):
        yy = 40 + k * 12
        regs += band(360, yy, 400 + k / 16.0, yy + 8, (160, 160, 160))
    gif([tag_ad(len(regs))] + regs)
    record(3, struct.pack('<QBBBBI', frame, 0, 0, 0, 0, 1))

hdr = bytearray(header)
struct.pack_into('<Q', hdr, 40, 0)  # first_frame 0
open(out, 'wb').write(bytes(hdr) + b''.join(records))
print('wrote', out, len(records), 'records')
