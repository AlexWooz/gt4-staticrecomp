#!/usr/bin/env python
"""Rebuild the Gran Turismo 4 Online Public Beta game ELF from its CORE.GT4 (TASK-100).

The beta disc (SCUS-97436, 2006-05-29) boots a small loader and keeps the game itself in an
encrypted ``CORE.GT4``.  Spec II ships that game already patched, as one plain ELF, so the
exact static patch set of Spec II is the byte diff between this rebuilt image and Spec II's
``SCUS_974.36`` (S1).  The rebuilt image is also the input for booting the unmodified beta
(S7a).

Confirmed container, recovered from the owner's loader (see docs/SPEC2-PATCHES.md):

    CORE.GT4 = [u64 LE nonce][Salsa20/20 stream ciphertext][u32 LE CRC-32C of everything before]
    plaintext = Salsa20/20(key = "PolyphonyDigital", nonce = that u64, counter = 0) XOR ciphertext
    plaintext = [u16 0x0101][u32 LE inflated size][raw deflate (-15)]
    inflated  = two u16-length-prefixed 128-byte header blobs, then i32 record count + u32 entry,
                then per record: u32 address, u32 size, body (the same container as retail v2.00)

Records (Confirmed): 24 bytes at 0x0065FBA4 (.reginfo image), the text at 0x00100000
(0x55FBBC bytes), the data at 0x0065FC00 (0xD45FC bytes).

The derived ELF reproduces the retail v2.00 layout recipe (scripts/ee_producer.py): text at file
0x1000 / VA 0x100000, .reginfo as its own PT_LOAD, data page-aligned, and a +8 MiB zero-fill
extent for the data segment (Hypothesis: the same BSS extent as Spec II and v2.00, which the
container does not carry).

The tool writes only to a fresh output directory and never copies game data into the repository.
"""

import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

ISO_NAME = "Gran Turismo 4 (USA) (Beta).iso"
ISO_SIZE = 2_654_109_696
ISO_SHA256 = "1f49b9d8077801188860b62c3efdbc631ba8af7df979373811ce8c60dcc48ebe"
CORE_PATH = "/CORE.GT4;1"
CORE_SIZE = 2_269_619
CORE_SHA256 = "0cf52c3ad8e9c1ddba55e7538a35dfed1c02cd0e38e5c348bdf80975046617ff"

SALSA_KEY = b"PolyphonyDigital"
SALSA_ROUNDS = 20
CONTAINER_TAG = 0x0101
MAX_INFLATED = 16 * 1024 * 1024
DATA_BSS_BYTES = 0x800000
R5900_FLAGS = 0x20924001
MAX_ELF = 16 * 1024 * 1024


def sha256(data):
    return hashlib.sha256(data).hexdigest()


# --- Salsa20/20 (tau constants: 16-byte key duplicated) ------------------------------------------------------------
def _rotl(v, n):
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF


def _salsa_quarter(x, a, b, c, d):
    x[b] ^= _rotl((x[a] + x[d]) & 0xFFFFFFFF, 7)
    x[c] ^= _rotl((x[b] + x[a]) & 0xFFFFFFFF, 9)
    x[d] ^= _rotl((x[c] + x[b]) & 0xFFFFFFFF, 13)
    x[a] ^= _rotl((x[d] + x[c]) & 0xFFFFFFFF, 18)


def salsa20_block(key, nonce, counter):
    if len(key) != 16:
        raise ValueError("this container uses the 16-byte (tau) variant")
    constants = b"expand 16-byte k"
    c0, c1, c2, c3 = struct.unpack("<4I", constants)
    k = struct.unpack("<4I", key)
    n = struct.unpack("<2I", nonce)
    ctr = struct.unpack("<2I", struct.pack("<Q", counter))
    # Standard Salsa20 state: constants at 0, 5, 10, 15 (tau: the key fills both halves).
    state = [c0, k[0], k[1], k[2], k[3], c1, n[0], n[1], ctr[0], ctr[1], c2, k[0], k[1], k[2], k[3], c3]
    x = state[:]
    for _ in range(SALSA_ROUNDS // 2):
        _salsa_quarter(x, 0, 4, 8, 12)
        _salsa_quarter(x, 5, 9, 13, 1)
        _salsa_quarter(x, 10, 14, 2, 6)
        _salsa_quarter(x, 15, 3, 7, 11)
        _salsa_quarter(x, 0, 1, 2, 3)
        _salsa_quarter(x, 5, 6, 7, 4)
        _salsa_quarter(x, 10, 11, 8, 9)
        _salsa_quarter(x, 15, 12, 13, 14)
    return struct.pack("<16I", *[((a + b) & 0xFFFFFFFF) for a, b in zip(x, state)])


def salsa20_stream_xor(key, nonce, data):
    out = bytearray(len(data))
    for offset in range(0, len(data), 64):
        block = salsa20_block(key, nonce, offset // 64)
        chunk = data[offset:offset + 64]
        out[offset:offset + len(chunk)] = bytes(a ^ b for a, b in zip(chunk, block))
    return bytes(out)


# --- CRC-32C (Castagnoli, as the loader computes it: MSB-first table) ---------------------------------------------
def _crc32c_table():
    table = []
    for i in range(256):
        crc = i << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1EDC6F41) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
        table.append(crc)
    return table


_CRC32C = _crc32c_table()


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC32C[((crc >> 24) ^ byte) & 0xFF]
    return crc ^ 0xFFFFFFFF


# --- Container ----------------------------------------------------------------------------------------------------
def decrypt_core(core):
    if len(core) < 12:
        raise ValueError("CORE.GT4 is truncated")
    nonce = core[:8]
    ciphertext = core[8:-4]
    trailer, = struct.unpack("<I", core[-4:])
    expected = crc32c(core[:-4])
    if trailer != expected:
        raise ValueError("CORE.GT4 CRC-32C mismatch: %08x != %08x" % (trailer, expected))
    return salsa20_stream_xor(SALSA_KEY, nonce, ciphertext), nonce, trailer


def inflate_container(plain):
    tag, size = struct.unpack_from("<HI", plain)
    if tag != CONTAINER_TAG:
        raise ValueError("unexpected CORE tag %04x" % tag)
    if size > MAX_INFLATED:
        raise ValueError("inflated size exceeds the supported limit")
    decoder = zlib.decompressobj(-15)
    try:
        payload = decoder.decompress(plain[6:], MAX_INFLATED + 1)
    except zlib.error as error:
        raise ValueError("invalid deflate stream") from error
    if len(payload) != size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise ValueError("deflate stream does not reproduce the declared size")
    return payload, size


def parse_container(payload):
    offset = 0
    headers = []
    for _ in range(2):
        length, = struct.unpack_from("<H", payload, offset)
        offset += 2
        headers.append(payload[offset:offset + length])
        offset += length
    count, entry = struct.unpack_from("<iI", payload, offset)
    offset += 8
    if not 0 < count <= 16:
        raise ValueError("implausible record count %d" % count)
    records = []
    for _ in range(count):
        address, size = struct.unpack_from("<II", payload, offset)
        offset += 8
        records.append({"address": address, "size": size, "body": payload[offset:offset + size]})
        offset += size
    if offset != len(payload):
        raise ValueError("%d trailing bytes after the records" % (len(payload) - offset))
    return headers, entry, records


def build_elf(entry, records):
    """Retail v2.00 / Spec II layout: text at 0x1000, .reginfo right after it, data page-aligned.

    The beta container also carries a 24-byte .reginfo record at 0x65FBA4, i.e. inside the last 24 bytes of the text
    range; Spec II stores those 24 bytes as text and its .reginfo at the text end (0x65FBBC). Both readings keep the
    text bytes at 0x65FBA4 (the container applies the text record after the reginfo), so the image is identical and we
    reproduce Spec II's non-overlapping file layout.
    """
    text = next((r for r in records if r["address"] == 0x100000), None)
    data = max(records, key=lambda r: r["address"])
    if text is None or data is text:
        raise ValueError("records do not describe a text and a data image")
    reginfo = [r for r in records if r is not text and r is not data]
    text_end = text["address"] + len(text["body"])
    file_offset = 0x1000
    parts = [(1, file_offset, text["address"], text["address"], len(text["body"]),
              len(text["body"]), 7, 4096, text["body"])]
    file_offset += len(text["body"])
    for record in reginfo:
        if len(record["body"]) != 24:
            raise ValueError("unexpected extra record of %d bytes at %08x" % (len(record["body"]), record["address"]))
        parts.append((0x70000000, file_offset, text_end, text_end, len(record["body"]),
                      len(record["body"]), 4, 4, record["body"]))
        file_offset += len(record["body"])
    file_offset = (file_offset + 0xFFF) & ~0xFFF
    parts.append((1, file_offset, data["address"], data["address"], len(data["body"]),
                  len(data["body"]) + DATA_BSS_BYTES, 6, 4096, data["body"]))
    file_offset += len(data["body"])
    if file_offset > MAX_ELF:
        raise ValueError("ELF size exceeds the supported limit")
    output = bytearray(file_offset)
    if 52 + 32 * len(parts) > len(output):
        raise ValueError("ELF header table exceeds the output size")
    output[:16] = b"\x7fELF\x01\x01\x01" + b"\0" * 9
    struct.pack_into("<HHIIIIIHHHHHH", output, 16, 2, 8, 1, entry, 52, 0, R5900_FLAGS, 52, 32,
                     len(parts), 0, 0, 0)
    for index, (ptype, offset, vaddr, paddr, filesz, memsz, flags, align, body) in enumerate(parts):
        struct.pack_into("<8I", output, 52 + index * 32, ptype, offset, vaddr, paddr, filesz, memsz, flags, align)
        output[offset:offset + filesz] = body
    return bytes(output)


def _word_map(data):
    """{address: word} for every PT_LOAD-backed word of an ELF image."""
    e_phoff, = struct.unpack_from("<I", data, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    words = {}
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _paddr, p_filesz, _memsz, _flags, _align = struct.unpack_from(
            "<8I", data, e_phoff + i * e_phentsize)
        if p_type != 1:
            continue
        for offset in range(0, p_filesz - 3, 4):
            words[p_vaddr + offset] = struct.unpack_from("<I", data, p_offset + offset)[0]
    return words


def read_core(iso_path):
    import pycdlib
    identity = {"size_bytes": iso_path.stat().st_size}
    digest = hashlib.sha256()
    with iso_path.open("rb") as stream:
        while chunk := stream.read(1 << 24):
            digest.update(chunk)
    identity["sha256"] = digest.hexdigest()
    if identity["size_bytes"] != ISO_SIZE or identity["sha256"] != ISO_SHA256:
        raise ValueError("unexpected ISO identity: %s" % identity)
    iso = pycdlib.PyCdlib()
    iso.open(str(iso_path))
    try:
        with iso.open_file_from_iso(iso_path=CORE_PATH) as stream:
            core = stream.read(CORE_SIZE + 1)
    finally:
        iso.close()
    if len(core) != CORE_SIZE or sha256(core) != CORE_SHA256:
        raise ValueError("unexpected CORE.GT4 identity")
    return core, identity


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--iso", type=Path, required=True)
    ap.add_argument("--output", type=Path, help="fresh output directory (private/sliceNNN-spec2-beta-elf)")
    ap.add_argument("--elf", type=Path, help="existing SCUS_974.36.elf produced by a previous run (diff mode)")
    ap.add_argument("--json", type=Path, help="write the machine-readable summary here")
    ap.add_argument("--check-spec2", type=Path, metavar="SCUS_974.36",
                    help="assert that the rebuilt image differs from this Spec II ELF only at 0x1001E8 and inside the "
                         "plugin segment")
    args = ap.parse_args(argv)

    summary = {"schema_version": 1, "schema": "spec2-beta-elf/1"}
    if args.elf:
        data = args.elf.read_bytes()
        summary["elf"] = {"path": args.elf.name, "size_bytes": len(data), "sha256": sha256(data)}
        payload = None
    else:
        core, iso = read_core(args.iso)
        summary["iso"] = iso
        summary["core"] = {"size_bytes": len(core), "sha256": sha256(core)}
        plain, nonce, trailer = decrypt_core(core)
        summary["crypto"] = {"cipher": "Salsa20/%d" % SALSA_ROUNDS, "key_bytes": 16,
                             "nonce": nonce[::-1].hex(), "crc32c": "%08x" % trailer}
        payload, inflated = inflate_container(plain)
        headers, entry, records = parse_container(payload)
        summary["container"] = {
            "sha256": sha256(payload), "size_bytes": len(payload), "entry": entry,
            "headers": [sha256(h) for h in headers],
            "records": [{"address": r["address"], "size": r["size"], "sha256": sha256(r["body"])}
                        for r in records],
        }
        data = build_elf(entry, records)
        summary["elf"] = {"size_bytes": len(data), "sha256": sha256(data)}
        if args.output:
            args.output.mkdir(parents=True, exist_ok=False)
            (args.output / "SCUS_974.36.elf").write_bytes(data)
            (args.output / "beta-core-payload.bin").write_bytes(payload)
            summary["elf"]["path"] = "SCUS_974.36.elf"
    if args.json:
        args.json.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    if args.check_spec2:
        beta_words = _word_map(data)
        spec2_words = _word_map(args.check_spec2.read_bytes())
        skip = (0x68A480, 0x68A480 + 0x2CCC)
        changed = sorted(a for a in set(beta_words) & set(spec2_words)
                         if beta_words[a] != spec2_words[a] and not (skip[0] <= a < skip[1]))
        summary["static_diff"] = {"addresses_outside_the_plugin": ["0x%08x" % a for a in changed]}
        if args.json:
            args.json.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print("static differences outside the plugin segment: %s" % summary["static_diff"]["addresses_outside_the_plugin"])
    print(json.dumps({k: v for k, v in summary.items() if k in ("iso", "core", "crypto", "elf")}, indent=2))
    if "container" in summary:
        print("records:", [(hex(r["address"]), r["size"]) for r in summary["container"]["records"]])
    return 0


if __name__ == "__main__":
    sys.exit(main())
