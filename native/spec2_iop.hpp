#pragma once

// TASK-100 S7b: the IOP reboot image rule of the Spec II target (scripts/spec2_iop_overlay.inc, capsule
// slice275-spec2-iop-runtime).
//
// At boot the game reboots the IOP with a Sony reboot image: retail v2.00 asks for
// "rom0:UDNL cdrom0:\IOPRP300.IMG;1", Spec II for "rom0:UDNL cdrom0:\DNAS310.IMG;1". Both files are the same kind of
// container (romdir: RESET/ROMDIR/EXTINFO and the same 16 system modules SYSMEM, LOADCORE, SIFCMD, SIFMAN, THREADMAN,
// IOMAN, MODLOAD, FILEIO, CDVDMAN, CDVDFSV, LOADFILE, TIMEMANI, ROMDRV, EESYNC, SYSCLIB, STDIO), and the runtime
// serves those modules as native HLE libraries instead of loading them, so the image changes no module set.
//
// What the image does select is the FILEIO protocol generation: each image embeds its LOADFILE version under the
// "PsIIloadfile" tag (IOPRP300 reports 3000, DNAS310 reports 3100), and the EE-side FILEIO library of that generation
// uses the asynchronous server 0x80000001 protocol, where a devctl (method 23) completes with the SIF interrupt packet
// 0x80000011. The donor enabled the asynchronous server only for a path containing "IOPRP", so the Spec II reboot fell
// back to the synchronous server: its devctl returned -1 and sent no interrupt packet, and the fileio client thread of
// the game then waited forever on the completion semaphore (docs/SPEC2-PLAN.md, S7b).
//
// The host decides by image name, exactly like the donor did, and logs the LOADFILE version it read from the image.
// v2.00 never asks for DNAS310.IMG, so its behaviour is unchanged even if this capsule were used there.

#include <cstdio>
#include <string>

namespace gt4::spec2_iop {

// Upper-case ASCII copy; the guest path is already upper case, but the rule should not depend on that.
inline std::string upper_ascii(const std::string &text) {
    std::string result = text;
    for (char &ch : result)
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return result;
}

inline bool contains_nocase(const std::string &text, const char *needle) {
    return upper_ascii(text).find(upper_ascii(needle)) != std::string::npos;
}

// True for the two Sony IOP reboot images the game uses (IOPRP300.IMG of v2.00 and DNAS310.IMG of Spec II).
inline bool async_fileio_boot_image(const std::string &path, const char *loadfile_version) {
    const bool ioprp = contains_nocase(path, "IOPRP");
    const bool dnas = contains_nocase(path, "DNAS310.IMG");
    if (!ioprp && !dnas) return false;
    std::fprintf(stderr, "[sif] %s is a Sony IOP reboot image (LOADFILE %.4s): asynchronous FILEIO server\n",
                 dnas ? "DNAS310.IMG" : "IOPRP image", loadfile_version);
    return true;
}

}  // namespace gt4::spec2_iop
