#!/usr/bin/env python
"""Map the Spec II plugin injector's boot-time patch set.

The Spec II image (SCUS-974.36) is a plain ELF: the Public Beta code plus a
``.plugin`` segment (VA 0x68A480, 0x2CCC bytes) that the game enters with one
call at 0x1001E8.  The plugin's entry routine writes its patches into guest
RAM at boot.  Static recompilation never executes those stores, so the patch
set has to be recovered ahead of time and applied to a derived ELF (S2).

This tool decodes the ELF, executes only the plugin's init routine on an
EE-RAM image (no game code is run: any call that leaves the plugin segment is
stubbed and recorded), and reports every store it makes.

Modes:
  --emit-json PATH   write the machine-readable patch table
  --markdown PATH    write a human-readable report
  --verify DERIVED   check that DERIVED equals the input plus every patch
  --perturb          re-run with different stack/v0 seeds and require the same
                     patch list (self-check for emulator assumptions)

The tool never writes game data: the ELF is read from the owner's ISO (or a
copied file), and only hashes, addresses and words appear in its output.
"""

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

PAYLOAD_VA = 0x68A480
PAYLOAD_SIZE = 0x2CCC
ENTRY_VA = 0x1001E8
RAM_SIZE = 0x2000000

TEXT_VA = 0x100000
TEXT_SIZE = 0x55FBBC
DATA_VA = 0x65FC00
DATA_SIZE = 0xD45FC

# Plugin helper routines, identified from the init routine.
HELPER_ZERO_WORD = 0x68A89C
HELPER_STORE_HALF = 0x68A8DC
HELPER_STORE_WORD = 0x68A918
HELPER_PATCH_JAL_NOP = 0x68A950
HELPER_PATCH_JAL = 0x68A794
HELPER_PATCH_J = 0x68A84C
HELPER_STORE_FLOAT = 0x68A814

OPCODE_NAMES = {
    0: "special", 1: "regimm", 2: "j", 3: "jal", 4: "beq", 5: "bne",
    6: "blez", 7: "bgtz", 8: "addi", 9: "addiu", 10: "slti", 11: "sltiu",
    12: "andi", 13: "ori", 14: "xori", 15: "lui", 32: "lb", 33: "lh",
    35: "lw", 36: "lbu", 37: "lhu", 39: "lwu", 40: "sb", 41: "sh",
    43: "sw", 47: "cache", 49: "lwc1", 51: "pref", 55: "ld", 57: "swc1",
    61: "sdc1", 63: "sd",
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class ElfImage:
    """Minimal ELF32 little-endian reader for one PT_LOAD image."""

    def __init__(self, data, name="SCUS_974.36"):
        self.data = data
        self.name = name
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise ValueError("not a little-endian ELF32 image")
        self.sha256 = sha256(data)
        self.size_bytes = len(data)
        e_phoff, = struct.unpack_from("<I", data, 28)
        e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
        e_shoff, = struct.unpack_from("<I", data, 32)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 46)
        self.segments = []
        for i in range(e_phnum):
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, _ = struct.unpack_from(
                "<IIIIIIII", data, e_phoff + i * e_phentsize)
            if p_type == 1:
                self.segments.append({
                    "offset": p_offset, "vaddr": p_vaddr, "filesz": p_filesz,
                    "memsz": p_memsz, "flags": p_flags})
        self.sections = []
        names_off = 0
        if e_shnum:
            shstr = struct.unpack_from("<I", data, e_shoff + e_shstrndx * e_shentsize + 16)[0]
            names_off = shstr
        for i in range(e_shnum):
            name_idx, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from(
                "<IIIIII", data, e_shoff + i * e_shentsize)
            end = data.index(b"\0", names_off + name_idx) if names_off else 0
            name = data[names_off + name_idx:end].decode("ascii", "replace") if names_off else ""
            self.sections.append({"name": name, "type": sh_type, "addr": sh_addr,
                                  "offset": sh_offset, "size": sh_size})

    def section(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        raise KeyError(name)

    def payload(self):
        sec = self.section(".plugin")
        return self.data[sec["offset"]:sec["offset"] + sec["size"]]

    def word_at(self, va):
        for seg in self.segments:
            if seg["vaddr"] <= va < seg["vaddr"] + seg["filesz"]:
                return struct.unpack_from("<I", self.data, seg["offset"] + va - seg["vaddr"])[0]
        raise ValueError("address %08x is not backed by the ELF file" % va)

    def image(self):
        """Return [(va, bytes)] for every PT_LOAD, in load order."""
        out = []
        for seg in self.segments:
            block = bytearray(seg["memsz"])
            block[:seg["filesz"]] = self.data[seg["offset"]:seg["offset"] + seg["filesz"]]
            out.append((seg["vaddr"], bytes(block)))
        return out

    def loaded_ranges(self):
        """File-backed address ranges as (start, end_exclusive, bytes)."""
        return [(s["vaddr"], s["vaddr"] + s["filesz"],
                 self.data[s["offset"]:s["offset"] + s["filesz"]]) for s in self.segments]

    def word_map(self):
        """{address: word} for every file-backed 4-byte-aligned word."""
        words = {}
        for start, end, body in self.loaded_ranges():
            for i in range(0, len(body) - 3, 4):
                words[start + i] = struct.unpack_from("<I", body, i)[0]
        return words



class ExternalCall(Exception):
    pass


class Interpreter:
    """Runs only the plugin segment; every branch out of it is an external call."""

    def __init__(self, elf, sp=0x01FFE000, zero_ret=True):
        self.elf = elf
        self.mem = bytearray(RAM_SIZE)
        for va, block in elf.image():
            self.mem[va:va + len(block)] = block
        self.r = [0] * 32
        self.r[29] = sp
        self.pc = PAYLOAD_VA
        self.r[31] = 0
        self.hi = 0
        self.lo = 0
        self.delay_target = None
        self.writes = []
        self.executed = set()
        self.calls = []
        self.zero_ret = zero_ret

    # -- memory ---------------------------------------------------------
    def load(self, addr, size, signed=False):
        addr &= 0xFFFFFFFF
        if addr + size > RAM_SIZE:
            raise RuntimeError("read outside EE RAM at %08x" % addr)
        return int.from_bytes(self.mem[addr:addr + size], "little", signed=signed)

    def store(self, addr, value, size):
        addr &= 0xFFFFFFFF
        if addr + size > RAM_SIZE:
            raise RuntimeError("write outside EE RAM at %08x" % addr)
        old = int.from_bytes(self.mem[addr:addr + size], "little")
        value &= (1 << (8 * size)) - 1
        self.mem[addr:addr + size] = value.to_bytes(size, "little")
        self.writes.append({"addr": addr, "size": size, "old": old, "new": value,
                            "pc": self.pc})

    # -- registers ------------------------------------------------------
    def get(self, n):
        return self.r[n] & 0xFFFFFFFF

    def set(self, n, v):
        if n:
            self.r[n] = v & 0xFFFFFFFFFFFFFFFF

    @staticmethod
    def s32(v):
        v &= 0xFFFFFFFF
        return v - 0x100000000 if v & 0x80000000 else v

    # -- control flow ---------------------------------------------------
    def run(self, max_steps=2_000_000):
        steps = 0
        while True:
            steps += 1
            if steps > max_steps:
                raise RuntimeError("step budget exhausted at %08x" % self.pc)
            if self.delay_target is not None:
                target, self.delay_target = self.delay_target, None
                pc = self.pc
                self.executed.add(pc)
                w = self.load(pc, 4)
                self.pc = (pc + 4) & 0xFFFFFFFF
                branch = self.execute(w, pc)
                if branch is not None:
                    raise RuntimeError("branch in delay slot at %08x" % pc)
                self.pc = target
                continue
            pc = self.pc
            if pc == 0:
                return steps
            self.executed.add(pc)
            w = self.load(pc, 4)
            self.pc = (pc + 4) & 0xFFFFFFFF
            target = self.execute(w, pc)
            if target is not None:
                self.delay_target = target
            if not self.in_plugin(self.pc) and self.delay_target is None:
                # Returned past the plugin entry: the init routine is done.
                if self.pc == 0:
                    return steps
                raise RuntimeError("left the plugin at %08x" % self.pc)

    @staticmethod
    def in_plugin(addr):
        return PAYLOAD_VA <= addr < PAYLOAD_VA + PAYLOAD_SIZE

    # -- execution ------------------------------------------------------
    def execute(self, w, pc):
        op = w >> 26
        rs = (w >> 21) & 0x1F
        rt = (w >> 16) & 0x1F
        rd = (w >> 11) & 0x1F
        sa = (w >> 6) & 0x1F
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 0:
            return self.special(w, rs, rt, rd, sa, pc)
        if op == 1:
            return self.regimm(w, rs, rt, pc)
        if op in (2, 3):
            target = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if op == 3:
                self.set(31, pc + 8)
            if not self.in_plugin(target):
                self.external_call(target, pc)
                return None
            return target
        if op == 4:
            return self.branch(self.get(rs) == self.get(rt), pc, simm)
        if op == 5:
            return self.branch(self.get(rs) != self.get(rt), pc, simm)
        if op == 6:
            return self.branch(self.s32(self.get(rs)) <= 0, pc, simm)
        if op == 7:
            return self.branch(self.s32(self.get(rs)) > 0, pc, simm)
        if op in (8, 9):
            self.set(rt, self.get(rs) + simm)
            return None
        if op == 10:
            self.set(rt, 1 if self.s32(self.get(rs)) < simm else 0)
            return None
        if op == 11:
            self.set(rt, 1 if self.get(rs) < (simm & 0xFFFFFFFF) else 0)
            return None
        if op == 12:
            self.set(rt, self.get(rs) & imm)
            return None
        if op == 13:
            self.set(rt, self.get(rs) | imm)
            return None
        if op == 14:
            self.set(rt, self.get(rs) ^ imm)
            return None
        if op == 15:
            self.set(rt, imm << 16)
            return None
        if op == 25:  # daddiu
            self.set(rt, self.r[rs] + simm)
            return None
        if op == 24:  # daddi
            self.set(rt, self.r[rs] + simm)
            return None
        if op == 32:
            self.set(rt, self.s32(self.load(self.get(rs) + simm, 1, True)))
            return None
        if op == 33:
            self.set(rt, self.load(self.get(rs) + simm, 2, True))
            return None
        if op == 35:
            self.set(rt, self.load(self.get(rs) + simm, 4))
            return None
        if op == 36:
            self.set(rt, self.load(self.get(rs) + simm, 1))
            return None
        if op == 37:
            self.set(rt, self.load(self.get(rs) + simm, 2))
            return None
        if op == 39:
            self.set(rt, self.load(self.get(rs) + simm, 4))
            return None
        if op == 40:
            self.store(self.get(rs) + simm, self.get(rt), 1)
            return None
        if op == 41:
            self.store(self.get(rs) + simm, self.get(rt), 2)
            return None
        if op == 43:
            self.store(self.get(rs) + simm, self.get(rt), 4)
            return None
        if op == 55:
            self.set(rt, self.load(self.get(rs) + simm, 8))
            return None
        if op == 63:
            self.store(self.get(rs) + simm, self.r[rt], 8)
            return None
        if op == 49:
            self.set(rt, self.load(self.get(rs) + simm, 4))
            return None
        if op == 57:
            self.store(self.get(rs) + simm, self.get(rt), 4)
            return None
        if op == 53:
            self.set(rt, self.load(self.get(rs) + simm, 8))
            return None
        if op == 61:
            self.store(self.get(rs) + simm, self.r[rt], 8)
            return None
        if op in (47, 51):
            return None
        if op == 17:
            return self.cop1(w, rs, rt)
        raise RuntimeError("unsupported opcode %02x at %08x (%08x)" % (op, pc, w))

    def external_call(self, target, pc):
        self.calls.append({"pc": pc, "target": target})
        if self.zero_ret:
            self.set(2, 0)

    def special(self, w, rs, rt, rd, sa, pc):
        f = w & 0x3F
        if f == 0:
            self.set(rd, self.get(rt) << sa)
        elif f == 2:
            self.set(rd, self.get(rt) >> sa)
        elif f == 3:
            self.set(rd, self.s32(self.get(rt)) >> sa)
        elif f == 4:
            self.set(rd, self.get(rt) << (self.get(rs) & 0x1F))
        elif f == 6:
            self.set(rd, self.get(rt) >> (self.get(rs) & 0x1F))
        elif f == 7:
            self.set(rd, self.s32(self.get(rt)) >> (self.get(rs) & 0x1F))
        elif f == 8:
            target = self.get(rs)
            if target == 0:
                return 0
            if not self.in_plugin(target):
                raise RuntimeError("jr outside the plugin at %08x -> %08x" % (pc, target))
            return target
        elif f == 9:
            target = self.get(rs)
            self.set(rd, pc + 8)
            if target == 0:
                return 0
            if not self.in_plugin(target):
                self.external_call(target, pc)
                return None
            return target
        elif f == 10:
            if self.get(rt) == 0:
                self.set(rd, self.get(rs))
        elif f == 11:
            if self.get(rt) != 0:
                self.set(rd, self.get(rs))
        elif f == 12 or f == 13:
            raise RuntimeError("trap at %08x" % pc)
        elif f == 16:
            self.set(rd, self.hi)
        elif f == 18:
            self.set(rd, self.lo)
        elif f in (24, 25):
            self.lo = self.get(rs) * self.get(rt)
        elif f == 26:
            a, b = self.s32(self.get(rs)), self.s32(self.get(rt))
            self.lo, self.hi = (a // b, a % b) if b else (0, 0)
        elif f == 27:
            a, b = self.get(rs), self.get(rt)
            self.lo, self.hi = (a // b, a % b) if b else (0, 0)
        elif f == 32:
            self.set(rd, self.get(rs) + self.get(rt))
        elif f == 33:
            self.set(rd, self.get(rs) + self.get(rt))
        elif f == 34:
            self.set(rd, self.get(rs) - self.get(rt))
        elif f == 35:
            self.set(rd, self.get(rs) - self.get(rt))
        elif f == 36:
            self.set(rd, self.get(rs) & self.get(rt))
        elif f == 37:
            self.set(rd, self.get(rs) | self.get(rt))
        elif f == 38:
            self.set(rd, self.get(rs) ^ self.get(rt))
        elif f == 39:
            self.set(rd, ~(self.get(rs) | self.get(rt)))
        elif f == 42:
            self.set(rd, 1 if self.s32(self.get(rs)) < self.s32(self.get(rt)) else 0)
        elif f == 43:
            self.set(rd, 1 if self.get(rs) < self.get(rt) else 0)
        elif f in (44, 45):
            self.set(rd, self.r[rs] + self.r[rt])
        elif f in (46, 47):
            self.set(rd, self.r[rs] - self.r[rt])
        else:
            raise RuntimeError("unsupported SPECIAL %02x at %08x (%08x)" % (f, pc, w))
        return None

    def regimm(self, w, rs, rt, pc):
        simm = w & 0xFFFF
        if simm & 0x8000:
            simm -= 0x10000
        if rt in (16, 17, 18, 19):
            self.set(31, pc + 8)
        kind = rt & 0x1F
        if kind == 0 or kind == 2:
            return self.branch(self.s32(self.get(rs)) < 0, pc, simm)
        if kind == 1 or kind == 3:
            return self.branch(self.s32(self.get(rs)) >= 0, pc, simm)
        raise RuntimeError("unsupported REGIMM rt=%d at %08x" % (rt, pc))

    def branch(self, taken, pc, simm):
        return (pc + 4 + (simm << 2)) & 0xFFFFFFFF if taken else None

    def cop1(self, w, rs, rt):
        if rs in (0, 2):
            return None
        if rs in (4, 6):
            return None
        raise RuntimeError("unsupported COP1 %08x" % w)


class CoreImage:
    """Minimal CORE-style image: address -> bytes from the container records."""

    def __init__(self, payload, name="CORE.GT4"):
        parsed = parse_core_payload(payload)
        self.name = name
        self.sha256 = sha256(payload)
        self.size_bytes = len(payload)
        self.strings = parsed["strings"]
        self.entry = parsed["entry"]
        self.records = parsed["records"]

    def word_map(self):
        words = {}
        for record in self.records:
            body = record["body"]
            for i in range(0, len(body) - 3, 4):
                words[record["address"] + i] = struct.unpack_from("<I", body, i)[0]
        return words

    def ranges(self):
        return [(r["address"], r["address"] + r["size"], r["body"]) for r in self.records]


def parse_core_payload(payload):
    """Container used by both retail v2.00 CORE.GT4 and (once decrypted) the beta."""
    off = 0
    strings = []
    for _ in range(2):
        if off + 2 > len(payload):
            raise ValueError("truncated CORE string header")
        length = struct.unpack_from("<H", payload, off)[0]
        off += 2
        if off + length > len(payload):
            raise ValueError("truncated CORE string")
        strings.append(payload[off:off + length])
        off += length
    if off + 8 > len(payload):
        raise ValueError("truncated CORE record header")
    count, entry = struct.unpack_from("<iI", payload, off)
    off += 8
    if not 0 < count <= 16:
        raise ValueError("implausible CORE record count %d" % count)
    records = []
    for _ in range(count):
        if off + 8 > len(payload):
            raise ValueError("truncated CORE record")
        address, size = struct.unpack_from("<II", payload, off)
        off += 8
        if off + size > len(payload):
            raise ValueError("CORE record extends beyond the payload")
        records.append({"address": address, "size": size, "body": payload[off:off + size]})
        off += size
    if off != len(payload):
        raise ValueError("%d trailing CORE bytes" % (len(payload) - off))
    return {"strings": strings, "entry": entry, "records": records}


def diff_words(old_words, new_words, skip_ranges=()):
    """Word-level differences between two images, coalesced into contiguous blocks."""
    addresses = sorted(set(old_words) & set(new_words))
    blocks = []
    current = None
    for address in addresses:
        if any(start <= address < end for start, end in skip_ranges):
            continue
        old, new = old_words[address], new_words[address]
        if old == new:
            current = None
            continue
        if current and current["address"] + current["words"] * 4 == address:
            current["words"] += 1
        else:
            current = {"address": address, "words": 1}
            blocks.append(current)
    for block in blocks:
        block["old_words"] = [old_words[block["address"] + i * 4] for i in range(block["words"])]
        block["new_words"] = [new_words[block["address"] + i * 4] for i in range(block["words"])]
        block["bytes"] = block["words"] * 4
    return blocks



VALID_OPCODES = set(OPCODE_NAMES) | set(range(16, 24)) | {17, 18, 24, 25, 28, 30, 31, 53, 54, 56, 58, 59, 61, 62}


def is_instruction(word):
    """True when the word decodes as a plausible MIPS III/MMI instruction."""
    return (word >> 26) in VALID_OPCODES


def classify(entry, elf, stack_top):
    addr = entry["addr"]
    size = entry["size"]
    new = entry["new"]
    if stack_top - 0x8000 <= addr <= stack_top:
        entry["region"] = "stack"
        entry["kind"] = "stack"
        return
    where = "text" if TEXT_VA <= addr < TEXT_VA + TEXT_SIZE else (
        "data" if DATA_VA <= addr < DATA_VA + DATA_SIZE else "payload" if PAYLOAD_VA <= addr < PAYLOAD_VA + PAYLOAD_SIZE else "other")
    entry["region"] = where
    if size != 4:
        entry["kind"] = "write%d" % (size * 8)
        return
    if where != "text":
        raw = new.to_bytes(4, "little")
        printable = all(32 <= b < 127 for b in raw)
        entry["kind"] = "string" if printable else "data_word"
        return
    op = new >> 26
    if op == 3:
        target = (addr + 4 & 0xF0000000) | ((new & 0x3FFFFFF) << 2)
        entry["target"] = target
        entry["kind"] = "jal_into_payload" if PAYLOAD_VA <= target < PAYLOAD_VA + PAYLOAD_SIZE else "jal"
    elif op == 2:
        target = (addr + 4 & 0xF0000000) | ((new & 0x3FFFFFF) << 2)
        entry["target"] = target
        entry["kind"] = "j_into_payload" if PAYLOAD_VA <= target < PAYLOAD_VA + PAYLOAD_SIZE else "j"
    elif new == 0:
        entry["kind"] = "zero"
    elif new == 0x03E00008:
        entry["kind"] = "jr_ra"
    elif op in (4, 5, 6, 7, 20, 21, 22, 23):
        entry["kind"] = "branch"
    elif is_instruction(new):
        entry["kind"] = "instruction:" + OPCODE_NAMES.get(op, "op%02x" % op)
    else:
        entry["kind"] = "data_word"


def run_injector(elf, sp=0x01FFE000, zero_ret=True):
    m = Interpreter(elf, sp=sp, zero_ret=zero_ret)
    steps = m.run()
    return m, steps


def build_patch_table(elf, sp=0x01FFE000):
    machine, steps = run_injector(elf, sp=sp)
    # Collapse repeated stores: the last value wins while the first old value is
    # the ELF content. The init writes each address at most once in practice.
    by_addr = {}
    order = []
    for w in machine.writes:
        key = (w["addr"], w["size"])
        if key not in by_addr:
            by_addr[key] = dict(addr=w["addr"], size=w["size"], old=w["old"],
                                new=w["new"], count=1, pcs=[w["pc"]])
            order.append(key)
        else:
            by_addr[key]["new"] = w["new"]
            by_addr[key]["count"] += 1
            by_addr[key]["pcs"].append(w["pc"])
    patches = [by_addr[k] for k in order]
    for p in patches:
        classify(p, elf, sp)
    live = [p for p in patches if p["region"] != "stack"]
    return machine, steps, live


def elf_from_iso(iso_path):
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.open(str(iso_path))
    try:
        with iso.open_file_from_iso(iso_path="/SCUS_974.36;1") as stream:
            data = stream.read()
    finally:
        iso.close()
    return data


NOTES = {
    0x1001E8: ("static trampoline: the beta called 0x005E3AA8 here; the payload calls it first "
               "(0x68A490) and then patches", "static", "High confidence"),
    0x121D58: ("hook: jal 0x004EFC00 -> payload 0x68B1DC (calls the original, then a two-entry "
               "callback table at 0x68D090)", "injector", "High confidence"),
    0x132E60: ("hook: jal 0x004F0650 -> payload 0x68B334", "injector", "High confidence"),
    0x14EC70: ("hook: jal 0x004F0B10 -> payload 0x68B744", "injector", "High confidence"),
    0x20B594: ("prologue rewrite: function at 0x0020B590 returns immediately (addiu sp -> jr ra); "
               "the delay slot stays lbu v0,0x1E60(v1)", "injector", "Confirmed"),
    0x20B744: ("Bnel -> bnez (0x54400006 -> 0x14000006): the delay slot now always runs",
               "injector", "Confirmed"),
    0x212D68: ("remove beql v0,zero (early-out) -> nop", "injector", "Confirmed"),
    0x212D70: ("lwc1 f0,0x51C(s0) -> mov.s f31,f2 (f31 used as a spare across the block)",
               "injector", "Confirmed"),
    0x212D74: ("c.olt.s f0,f20 -> c.olt.s f2,f1 (compare the gear-ratio pair instead)",
               "injector", "Hypothesis"),
    0x212D7C: ("lwc1 f2,0x510(s0) -> lwc1 f31,0x510(s0)", "injector", "Confirmed"),
    0x212D80: ("sb zero,0x620(s0) -> mov.s f31,f13", "injector", "Confirmed"),
    0x212D84: ("b 0x212F28 -> nop (fall through instead of jumping)", "injector", "Confirmed"),
    0x212DA0: ("mov.s f2,f23 -> mov.s f31,f23", "injector", "Confirmed"),
    0x212DC0: ("sub.s f20,f11,f22 -> sub.s f20,f31,f22", "injector", "Confirmed"),
    0x23F438: ("hook: jal 0x0023F370 -> payload 0x68ABC4 (6-entry table lookup at 0x68D020, "
               "returns the index or -1)", "injector", "High confidence"),
    0x23F444: ("hook: jal 0x0023F3B0 -> payload 0x68ADB4", "injector", "High confidence"),
    0x240254: ("beq s2,zero,0x240274 (0x12400007) -> nop", "injector", "Confirmed"),
    0x27EAD8: ("addiu a1,zero,4 -> addiu a1,zero,11 (0x24050004 -> 0x2405000B)", "injector",
               "Confirmed"),
    0x5225F0: ("jal 0x005D5E98 (0x0C1757A6) -> nop", "injector", "Confirmed"),
    0x6605F4: ("u32 flag 3 -> 1 (no reader found in a text scan)", "injector", "Unknown"),
    0x6F02B0: ("sound bank name table: normal0..normal3 -> exh0..exh3", "injector", "Confirmed"),
    0x6F02B4: ("sound bank name table tail: 'al0\\0' -> 0 (8-byte entries)", "injector", "Confirmed"),
    0x6F02B8: ("sound bank name table: normal1 -> exh1", "injector", "Confirmed"),
    0x6F02BC: ("sound bank name table tail: 'al1\\0' -> 0", "injector", "Confirmed"),
    0x6F02C0: ("sound bank name table: normal2 -> exh2", "injector", "Confirmed"),
    0x6F02C4: ("sound bank name table tail: 'al2\\0' -> 0", "injector", "Confirmed"),
    0x6F02C8: ("sound bank name table: normal3 -> exh3", "injector", "Confirmed"),
    0x6F02CC: ("sound bank name table tail: 'al3\\0' -> 0", "injector", "Confirmed"),
    0x6F02D0: ("sound bank name table: turbo0 -> exh0", "injector", "Confirmed"),
    0x6F02D4: ("sound bank name table tail: 'o0\\0' -> 0", "injector", "Confirmed"),
    0x6F02D8: ("sound bank name table: turbo1 -> exh1", "injector", "Confirmed"),
    0x6F02DC: ("sound bank name table tail: 'o1\\0' -> 0", "injector", "Confirmed"),
    0x6F02E0: ("sound bank name table: turbo2 -> exh2", "injector", "Confirmed"),
    0x6F02E4: ("sound bank name table tail: 'o2\\0' -> 0", "injector", "Confirmed"),
    0x6F02E8: ("sound bank name table: turbo3 -> exh3", "injector", "Confirmed"),
    0x6F02EC: ("sound bank name table tail: 'o3\\0' -> 0", "injector", "Confirmed"),
}


def annotate(patch):
    note = NOTES.get(patch["addr"])
    if note:
        patch["note"], patch["source"], patch["confidence"] = note
    else:
        patch.setdefault("note", "")
        patch.setdefault("source", "injector")
        patch.setdefault("confidence", "Unknown")
    return patch


def render_markdown(document, beta_info):
    lines = []
    lines.append("| Address | Region | Old | New | Kind | Note |")
    lines.append("|---|---|---|---|---|---|")
    for p in document["patches"]:
        lines.append("| `0x%08X` | %s | `0x%08X` | `0x%08X` | %s | %s |" % (
            p["addr"], p["region"], p["old"], p["new"], p.get("kind", ""), p.get("note", "")))
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--iso", type=Path)
    src.add_argument("--elf", type=Path)
    ap.add_argument("--beta-payload", type=Path,
                    help="plain CORE container payload recovered from the beta's CORE.GT4")
    ap.add_argument("--emit-json", type=Path)
    ap.add_argument("--markdown", type=Path)
    ap.add_argument("--verify", type=Path, help="derived ELF to check")
    ap.add_argument("--perturb", action="store_true")
    args = ap.parse_args(argv)

    if args.iso:
        data = elf_from_iso(args.iso)
        source = {"iso": args.iso.name}
    else:
        data = args.elf.read_bytes()
        source = {"elf": args.elf.name}
    elf = ElfImage(data)
    machine, steps, patches = build_patch_table(elf)
    for p in patches:
        annotate(p)

    if args.perturb:
        base = [(p["addr"], p["size"], p["new"]) for p in patches]
        for sp, zero in ((0x01FFF000, True), (0x01F00000, False), (0x01FF8000, True)):
            _, _, alt_patches = build_patch_table(elf, sp=sp)
            alt = [(p["addr"], p["size"], p["new"]) for p in alt_patches]
            if alt != base:
                raise SystemExit("patch table changes with sp=%08x" % sp)
        print("perturbation check: identical patch table for 3 stack/v0 seeds")

    document = {
        "schema_version": 1,
        "target": "gran-turismo-4-spec-ii-v1.10",
        "elf": {"name": elf.name, "size_bytes": elf.size_bytes, "sha256": elf.sha256},
        "entry": {"call_address": ENTRY_VA, "thunk": 0x68A768, "init": PAYLOAD_VA},
        "injector": {
            "executed_instructions": len(machine.executed),
            "steps": steps,
            "external_calls": [{"pc": c["pc"], "target": c["target"]} for c in machine.calls],
        },
        "patches": sorted(patches, key=lambda p: p["addr"]),
    }
    document["counts"] = {
        "total": len(patches),
        "text": sum(1 for p in patches if p["region"] == "text"),
        "data": sum(1 for p in patches if p["region"] == "data"),
        "payload": sum(1 for p in patches if p["region"] == "payload"),
    }

    if args.beta_payload:
        beta_payload = args.beta_payload.read_bytes()
        beta = CoreImage(beta_payload)
        skip = [(PAYLOAD_VA, PAYLOAD_VA + PAYLOAD_SIZE)]
        blocks = diff_words(beta.word_map(), elf.word_map(), skip_ranges=skip)
        document["beta"] = {
            "container_sha256": sha256(beta_payload),
            "entry": beta.entry,
            "headers": [{"size_bytes": len(s), "sha256": sha256(s)} for s in beta.strings],
            "records": [{"address": r["address"], "size": r["size"],
                         "sha256": sha256(r["body"])} for r in beta.records],
        }
        document["static_patches"] = {
            "blocks": len(blocks),
            "words": sum(b["words"] for b in blocks),
            "detail": [{"address": b["address"], "words": b["words"],
                        "old_words": b["old_words"], "new_words": b["new_words"]} for b in blocks],
        }

    if args.verify:
        derived = args.verify.read_bytes()
        if len(derived) != len(elf.data):
            raise SystemExit("derived ELF has a different size")
        for p in patches:
            off = None
            for seg in elf.segments:
                if seg["vaddr"] <= p["addr"] < seg["vaddr"] + seg["filesz"]:
                    off = seg["offset"] + p["addr"] - seg["vaddr"]
            if off is None:
                raise SystemExit("patch address %08x is not file-backed" % p["addr"])
            got = int.from_bytes(derived[off:off + p["size"]], "little")
            want = p["new"] & ((1 << (8 * p["size"])) - 1)
            if got != want:
                raise SystemExit("derived ELF mismatch at %08x: %08x != %08x" % (p["addr"], got, want))
        print("verify: %d patches present in %s" % (len(patches), args.verify.name))

    if args.emit_json:
        args.emit_json.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        print("wrote %s (%d injector patches)" % (args.emit_json, len(patches)))
    if args.markdown:
        args.markdown.write_text(render_markdown(document, document.get("beta")), encoding="utf-8")
        print("wrote %s" % args.markdown)
    if not args.emit_json and not args.markdown:
        print(json.dumps(document["counts"]))
        for p in document["patches"]:
            print("%08x %-8s %06x -> %08x %-22s %s" % (
                p["addr"], p["region"], p["old"], p["new"], p.get("kind", ""), p.get("note", "")))
        if "static_patches" in document:
            print("static: %d blocks, %d words" % (
                document["static_patches"]["blocks"], document["static_patches"]["words"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
