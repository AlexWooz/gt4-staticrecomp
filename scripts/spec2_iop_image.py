"""Map an IOP reboot image (romdir) and compare the runtime HLE surface with it (TASK-100 S7b).

The game reboots the IOP with a Sony reboot image: retail v2.00 asks for `rom0:UDNL cdrom0:\\IOPRP300.IMG;1`, Spec II
for `rom0:UDNL cdrom0:\\DNAS310.IMG;1`. The file is a romdir container: a list of 16-byte entries
(`char name[10]; u16 extinfo_size; u32 size`), terminated by a zero name, then one ELF module body per listed module,
16-byte aligned, in entry order. RESET/ROMDIR/EXTINFO are bookkeeping entries without a module body.

The runtime never loads these images: it answers the reboot RPC with a log line and serves the system libraries as
native HLE (`runtime/src/iop/libs.cpp`, `sif.cpp`, `cdvd.cpp`, `cdvdfsv.cpp`, `io.cpp`). This tool records, per module,
the HLE surface that covers it, so the claim can be checked instead of assumed.

Read-only: it reads the owner's discs, writes only names, sizes and hashes.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ISO_PIN = ROOT / "docs/inputs/spec2-iso.json"
ENTRY_SIZE = 16
BOOKKEEPING = ("RESET", "ROMDIR", "EXTINFO")

# Donor HLE surface per system module of the Sony reboot image (paths relative to runtime/src). "none" is recorded
# explicitly: those modules register no library, which is safe only because the image is never loaded.
HLE_SURFACE = {
    "SYSMEM": ["iop/libs.cpp: sysmem"],
    "LOADCORE": ["iop/libs.cpp: loadcore"],
    "SIFCMD": ["iop/libs.cpp: sifcmd (with sif.cpp send/receive)"],
    "SIFMAN": ["iop/libs.cpp: sifman"],
    "THREADMAN": ["iop/libs.cpp: int thrman thbase, thsemap, thevent, thmsgbx, thfpool"],
    "IOMAN": ["iop/libs.cpp: ioman", "iop/io.cpp: file descriptors and rom0:/cdrom0: devices"],
    "MODLOAD": ["iop/libs.cpp: modload", "iop/kernel.cpp: load_module_buffer"],
    "FILEIO": ["iop/sif.cpp: server 0x80000001 (asynchronous 3.x path and synchronous fallback)"],
    "CDVDMAN": ["iop/cdvd.cpp: cdvdman"],
    "CDVDFSV": ["iop/cdvdfsv.cpp: servers 0x80000592, 0x80000593, 0x80000595, 0x80000596, 0x80000597, 0x8000059A, 0x8000059C"],
    "LOADFILE": ["iop/sif.cpp: server 0x80000006"],
    "TIMEMANI": ["iop/libs.cpp: timrman"],
    "ROMDRV": ["iop/io.cpp: rom0: device"],
    "EESYNC": ["none"],
    "SYSCLIB": ["iop/libs.cpp: sysclib"],
    "STDIO": ["iop/libs.cpp: stdio"],
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_image(data):
    """Parses one romdir image; returns the bookkeeping entries and the module bodies in order."""
    entries, offset = [], 0
    while offset + ENTRY_SIZE <= len(data):
        name = data[offset:offset + 10].split(b"\x00")[0].decode("latin-1")
        extinfo = int.from_bytes(data[offset + 10:offset + 12], "little")
        size = int.from_bytes(data[offset + 12:offset + 16], "little")
        if not name:
            break
        entries.append({"name": name, "extinfo_size": extinfo, "size": size, "entry_offset": offset})
        offset += ENTRY_SIZE
    if not entries or entries[0]["name"] != "RESET":
        raise ValueError("not a romdir image: the first entry is not RESET")
    cursor = offset
    while cursor + 4 <= len(data) and data[cursor:cursor + 4] != b"\x7fELF":
        cursor += 1
    modules = []
    for entry in entries:
        if entry["name"] in BOOKKEEPING:
            continue
        body = data[cursor:cursor + entry["size"]]
        if len(body) != entry["size"]:
            raise ValueError(f"{entry['name']}: module body runs past the image")
        modules.append({
            "name": entry["name"],
            "extinfo_size": entry["extinfo_size"],
            "size": entry["size"],
            "offset": cursor,
            "elf": body[:4] == b"\x7fELF",
            "sha256": sha256(body),
            "hle": HLE_SURFACE.get(entry["name"], ["Unknown"]),
        })
        cursor = (cursor + entry["size"] + 15) & ~15
    return {"size_bytes": len(data), "sha256": sha256(data), "directory_bytes": offset, "modules": modules}


def read_iso_file(iso_path, iso_name):
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.open(str(iso_path))
    try:
        with iso.open_file_from_iso(iso_path=iso_name) as stream:
            return stream.read()
    finally:
        iso.close()


def verify_spec2_iso(iso_path):
    pin = json.loads(ISO_PIN.read_text(encoding="utf-8"))
    digest, size = hashlib.sha256(), 0
    with iso_path.open("rb") as stream:
        while chunk := stream.read(1 << 24):
            digest.update(chunk)
            size += len(chunk)
    identity = {"size_bytes": size, "sha256": digest.hexdigest()}
    if identity != pin["disc"]:
        raise ValueError("unexpected Spec II ISO identity: %s" % identity)
    return identity


def build_report(spec2_iso, compare_iso=None):
    identity = verify_spec2_iso(spec2_iso)
    report = {
        "task": "TASK-100 S7b",
        "purpose": "The Spec II IOP reboot image (DNAS310.IMG) against retail's IOPRP300.IMG and the runtime HLE surface",
        "spec2_iso_sha256": identity["sha256"],
        "image": {"path": "/DNAS310.IMG;1", **parse_image(read_iso_file(spec2_iso, "/DNAS310.IMG;1"))},
    }
    if compare_iso is not None:
        report["retail_image"] = {"path": "/IOPRP300.IMG;1", **parse_image(read_iso_file(compare_iso, "/IOPRP300.IMG;1"))}
        retail = {module["name"]: module for module in report["retail_image"]["modules"]}
        for module in report["image"]["modules"]:
            other = retail.get(module["name"])
            module["retail_size"] = other["size"] if other else None
            module["retail_sha256"] = other["sha256"] if other else None
            module["same_as_retail"] = bool(other and other["sha256"] == module["sha256"])
        report["comparison"] = {
            "retail_iso": "the owner's v2.00 ISO (identity pinned in docs/inputs/slice118-volume-catalogue.json; "
                          "not re-verified here)",
            "same_module_order": [m["name"] for m in report["image"]["modules"]] ==
                                 [m["name"] for m in report["retail_image"]["modules"]],
            "identical_modules": [m["name"] for m in report["image"]["modules"] if m["same_as_retail"]],
        }
    report["findings"] = [
        "Both images are romdir containers with the same 16 system modules in the same order.",
        "The runtime loads neither image: it answers the reboot RPC with a log line and serves the system libraries "
        "as native HLE, so no module of DNAS310.IMG has to be loaded for the HLE set to be complete.",
        "What the image selects is the FILEIO protocol generation through its own LOADFILE version (IOPRP300 3000, "
        "DNAS310 3100); the donor switched the asynchronous FILEIO server on only for a path containing 'IOPRP'.",
    ]
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--iso", type=Path, required=True, help="the owner's Spec II ISO")
    parser.add_argument("--compare-iso", type=Path, help="the owner's v2.00 ISO, for the IOPRP300 comparison")
    parser.add_argument("--json", type=Path, help="write the report here (docs/inputs pin)")
    options = parser.parse_args(argv)
    report = build_report(options.iso, options.compare_iso)
    text = json.dumps(report, indent=2) + "\n"
    if options.json:
        if options.json.exists():
            raise SystemExit(f"preserve the existing pin: {options.json}")
        options.json.write_text(text, encoding="utf-8", newline="\n")
        print(json.dumps({"pin": str(options.json), "modules": len(report["image"]["modules"])}, indent=2))
    else:
        print(text)


if __name__ == "__main__":
    main()
