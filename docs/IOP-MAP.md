# IOP Module Map

2026-10-14, slice 151. Read-only study; nothing changed.

## Confirmed

- Boot reboots the IOP with `cdrom0:\IOPRP300.IMG` (16 Sony modules plus the RESET/ROMDIR/EXTINFO records: SYSMEM,
  LOADCORE, SIFCMD, SIFMAN, THREADMAN, IOMAN, MODLOAD, FILEIO, CDVDMAN, CDVDFSV,
  LOADFILE, TIMEMANI, ROMDRV, EESYNC, SYSCLIB, STDIO). The donor serves these
  libraries as native HLE functions (`runtime/src/iop/libs.cpp`, `io.cpp`,
  `cdvd.cpp`, `sif.cpp`).
- GT4 then loads 23 IRX files from `cdrom0:\IRX\`, identical in all 15 owner
  sessions and at identical load addresses (669,395 bytes total):
  - network: INET, NETCNF, RTINETB, INETCTL, DEV9, SMAP;
  - sound: LIBSD, PDISPU2, PDISTR, LIBPDI; disc: PDICDVD;
  - memory card: MCMAN, MCSERV, SIO2MAN, SIO2D;
  - USB: USBD, PDIUSB, USTORAGE; other: LGDEV, DBCMAN, DS2U_D, MSIFRPC, POWOFF.
  PADMAN, MTAPMAN, EYETOY and PDINETCNF are on disc but never loaded; pads go
  through the native SIO2 model.
- `register_hle_module` (`kernel.cpp:484`) has no caller, so every game IRX is
  interpreted. Imports bind to HLE traps or guest library jumps.
- The donor IOP interpreter writes load results immediately: the R3000A
  one-instruction load delay is not modeled (`iop/cpu.cpp` load cases). Fidelity
  gap independent of AOT; impact on GT4 Unknown.
- Measured cost: `iop_run` about 0.4 ms per race frame. No per-module counter.

## Plan

1. Add per-module instruction counters and per-library HLE call counts.
2. Only if a module group dominates: native HLE of that group (likely the sound
   path LIBSD/PDISPU2/PDISTR), keeping the SPU2, SIO2 and CDVD contracts. This is
   simpler than a full R3000A translator.
3. Full IOP AOT (load delay, COP0, thread yield points, per-base relocation —
   bases are fixed for this ISO) is possible but weeks of work; low priority.

## Spec II (TASK-100 S6)

Source: the owner's Spec II ISO. Pin: [inputs/spec2-iop.json](inputs/spec2-iop.json) (35 files, 3,747,861 bytes,
every size and SHA-256). Tool contract: `tests/python/test_spec2.py` (IopTests).

**The disc** [Confirmed]. Spec II carries 30 files in `\IRX\` and 5 in `\NET\`. Against v2.00: 6 are new
(`LGAUD.IRX`, `PPP.IRX`, `PPPOE.IRX`, `DNAS_MC.ERX`, `DNAS_NET.ERX`, `NTG_ISCK.ERX`), 5 are byte-identical
(`LIBPDI`, `PDICDVD`, `PDINETCNF`, `PDIUSB` and the two `/NET` icon files), and the rest are different builds. The
beta shares Spec II's `\IRX\` set exactly, so the extra modules come with the Public Beta lineage, not from the
Spec II patch.

**The load list** [Confirmed strings, order Unknown]. `SCUS_974.36` contains these lower-case names:
`pad2/ds2u_d.irx`, `sio2man`, `mcman`, `mcserv`, `sio2d`, `dbcman`, `libsd`, `usbd`, `inet`, `netcnf`, `inetctl`,
`smap`, `ppp`, `pppoe`, `msifrpc`, `libpdi`, `pdicdvd`, `pdispu2`, `pdistr`, `pdiusb`, `ustorage`, `lgdev`, `dev9`,
`powoff`, `rtinetb`, `eyetoy`, `pdinetcnf`, `lgaud`. That is 28 names against v2.00's 23 loaded modules: the new
ones are `ppp`, `pppoe` and `lgaud` (plus `dev9`/`smap`/`rtinetb`, which v2.00 also loads). Whether every name is
loaded on the offline path, and in what order, is Unknown until S7b.

**Policy per module** (fallbacks in order, no JIT anywhere):

| Group | Policy | Why |
|---|---|---|
| Pads, memory card, disc, USB, sound, `POWOFF`, `LGAUD` | interpreted as today | already covered by the HLE system libraries and the SIO2/SPU2/CDVD contracts |
| `INET`, `NETCNF`, `INETCTL`, `SMAP`, `RTINETB`, `DEV9`, `PPP`, `PPPOE` | interpreted, with no device model | no DEV9/SMAP/PPP hardware exists in the runtime; the module loads and its device open must fail cleanly. A hang is an S7b finding: first a per-module instruction counter, then an HLE stub that reports "no network device", then refusal to load |
| `DNAS_MC.ERX`, `DNAS_NET.ERX`, `NTG_ISCK.ERX` (under `\NET`) | not loaded offline | DNAS authentication needs a network and a server; offline play must not reach them |
| `PADMAN`, `MTAPMAN`, `EYETOY`, `PDINETCNF` | on disc, not requested | v2.00 never loads them; pads go through the native SIO2 model |

Nothing here changes v2.00: the module files come from the disc the user supplies and the interpreter is the existing
one. The S6 acceptance ("offline play without hangs") stays open until S7b runs the boot.

## Spec II — the IOP reboot image (TASK-100 S7b)

Pin: [inputs/spec2-dnas310.json](inputs/spec2-dnas310.json) (sizes and hashes only). Tool: `scripts/spec2_iop_image.py`.

**Both games reboot the IOP with a Sony reboot image** [Confirmed]. v2.00 asks for
`rom0:UDNL cdrom0:\IOPRP300.IMG;1`; Spec II asks for `rom0:UDNL cdrom0:\DNAS310.IMG;1`. The two files are the same
container: a romdir (`RESET`, `ROMDIR`, `EXTINFO` then one 16-byte entry per module, bodies 16-byte aligned, each an
ELF IOP module), 278,465 and 282,545 bytes, with **the same 16 system modules in the same order**:

SYSMEM, LOADCORE, SIFCMD, SIFMAN, THREADMAN, IOMAN, MODLOAD, FILEIO, CDVDMAN, CDVDFSV, LOADFILE, TIMEMANI, ROMDRV,
EESYNC, SYSCLIB, STDIO.

No module is byte-identical between the two images (they are different builds); SIFMAN, CDVDMAN, CDVDFSV and EESYNC
also differ in size. The runtime loads **neither** image: it logs the reboot request and serves those system
libraries as native HLE (the table in the pin names the donor file per module), which is why v2.00 never needed
IOPRP300.IMG on disk. So "loading DNAS310.IMG" cannot mean loading its modules; the HLE set is already the answer.

**What the image does change is the FILEIO protocol generation** [Confirmed by the client code]: each image embeds
its LOADFILE version under the `PsIIloadfile` tag — IOPRP300 reports `3000`, DNAS310 reports `3100` — and the
runtime returns that word to the EE (LOADFILE server 0x80000006, method 0x09). The EE-side FILEIO library of that
generation uses the asynchronous protocol of server 0x80000001: `devctl` (method 23) completes with the SIF packet
`0x80000011`, not with the ordinary RPC end packet `0x8000000A`. The donor selected the asynchronous server only
when the reboot path contained `IOPRP`, so the Spec II boot fell back to the synchronous server: the `devctl` on the
DEV9 device returned -1, no `0x80000011` packet was sent, and the game thread that waits on the per-call FILEIO
semaphore never woke (S7b stall; the semaphore dump shows the waiters). The fix is the runtime overlay capsule
`slice276-spec2-iop-runtime` (`scripts/spec2_iop_overlay.inc` + `native/spec2_iop.hpp`): a DNAS310.IMG reboot selects
the same asynchronous FILEIO server as IOPRP300.IMG. v2.00 never asks for that image, so its behaviour is unchanged.

**SIF servers seen at boot** [Confirmed unless noted]: server `0x80000400` is registered by the guest **MCSERV**
module (its init calls `sceSifRegisterRpc(&cd, 0x80000400, …)`), and the game binds it from `0x005B88D4` (Spec II;
`sceSifBindRpc(&cd, 0x80000400, 0)`), the memory-card service it polls while a card is present. That polling is not
what blocks the boot: the EE threads wait on the FILEIO semaphore above, not on an MC reply.

