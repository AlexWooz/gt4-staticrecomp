// Hardened VU1 offline emitter. Derived from recomp/vurecomp.cpp of the MIT
// ps2-web-recomp donor, commit 1b4808793a12c45125262fd2d5f50a9ec9f7c020:
//   Copyright (c) 2026 Samir Nuri, MIT License (see MIT-LICENSE.txt).
//
// Changes from the donor emitter:
//   - input files: exact 16384-byte reads, strict KEY_ENTRY.bin names, key
//     recomputed and checked, fail-closed on anything unexpected;
//   - output: fresh directories only, nothing is deleted;
//   - identity: SHA-256 over entry and reachable instruction words, and
//     refusal of equal keys with different identities;
//   - bounded execution with an explicit interpreter fallback (see
//     vu_recomp_contract.hpp), instead of unbounded goto loops;
//   - unknown upper or lower encodings exit to the interpreter and are counted;
//     they are never emitted as NOPs.

#include "vurecomp.hpp"

#include "program_identity.hpp"
#include "sha256.hpp"
#include "ps2/vu_ops.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace gt4::vurecomp {

// Emitter profiles. Unit 1 reproduces the VU1 output of slices 151-157 exactly.
const UnitProfile &unit_profile(int unit) {
    static const UnitProfile vu1{};
    static const UnitProfile vu0 = [] {
        UnitProfile profile;
        profile.unit = 0;
        profile.image_bytes = 4096;
        profile.uploaded_bytes = 0xA80;
        profile.data_qwords = 256;
        profile.xgkick_allowed = false;
        profile.exact_flags = true;
        profile.file_prefix = "vu0_generated";
        profile.function_prefix = "vu0_";
        profile.identity_tag = "GT4-VU0-PROGRAM-IDENTITY-1";
        return profile;
    }();
    return unit == 0 ? vu0 : vu1;
}

namespace {

constexpr std::uint32_t kMask = kCodeMask;
constexpr int kAcc = 32;
constexpr std::uint32_t kVnopUpperWithoutFlags = 0x0000033Cu;
constexpr std::uint32_t kEndBit = 0x40000000u;
constexpr std::uint32_t kImmediateBit = 0x80000000u;


std::uint32_t word(const std::vector<std::uint8_t> &code, std::uint32_t at) {
    std::uint32_t v;
    std::memcpy(&v, code.data() + (at & (code.size() - 1)), 4);
    return v;
}


std::string label(std::uint32_t pc) { return std::format("L_{:04X}", pc); }

const char *const kLane = "xyzw";

bool lane(unsigned dest, int i) { return dest & (8u >> i); }

// Which observable results of one instruction must be computed. The donor
// interpreter keeps MAC/status/clip for every write; the emitted code keeps only
// what a later reader or the program exit can see (see FunctionGenerator::observe).
struct FlagUse {
    bool full = true;         // donor behaviour: flags() with status, every write
    bool mac_reader = true;   // MAC pushed to the FlagPipe for a later MAC reader
    bool mac_exit = true;     // MAC kept in last_mac for the program exit
    bool clip_reader = true;  // clip pushed to the FlagPipe for a later clip reader (the value is always kept)
};

struct Upper {
    std::string pre, post;
    bool sets_flags = false;  // flag-setting arithmetic (MAC writer), independent of FlagUse
    bool clip = false;        // clip writer (upper CLIP), independent of FlagUse
    bool known = true;  // false for encodings the donor decoder would silently treat as NOP
    int target = -1;
    unsigned mask = 0;
    bool normalized = false;
    std::vector<int> shadow_reads;
};

std::string vf(unsigned i, int lane_index) {
    if (!i) return lane_index == 3 ? "0x3F800000u" : "0u";
    return std::format("r.vf[{}].w[{}]", i, lane_index);
}

std::string vreg(unsigned i) { return i ? std::format("vload(r.vf[{}])", i) : "v4u{0u, 0u, 0u, 0x3F800000u}"; }

std::string shadow(int reg) { return reg == kAcc ? "nacc" : std::format("n{}", reg); }

std::string lane_mask(unsigned d) {
    std::string mask = "v4u{";
    for (int i = 0; i < 4; ++i) mask += std::string(i ? ", " : "") + (lane(d, i) ? "0xFFFFFFFFu" : "0u");
    return mask + "}";
}

// MAC update for one flag-setting result over its destination lanes. Empty when
// no MAC value is observable, so the arithmetic is emitted without flag work.
std::string mac_update(const std::string &value, unsigned d, const FlagUse &use) {
    if (use.full) return std::format("flags(r, {}, {}u); fl.set_mac(r.mac, r.status); ", value, d);
    if (!use.mac_reader && !use.mac_exit) return {};
    std::string s = std::format("{{ const std::uint32_t m = mac_of_result({}, {}u); ", value, d);
    if (use.mac_reader) s += "fl.set_mac(m, r.status); ";
    if (use.mac_exit) s += "last_mac = m; ";
    return s + "} ";
}

// donor_exact selects the donor exact interpreter arithmetic of run(0, pc) (VU0): per-lane fp::add,
// fp::sub, fp::mul, fp::from_fixed and fp::to_fixed with input flushing, in scalar lane code. VU1 keeps
// the SIMD helpers of run_fast (unchanged output).
Upper upper(std::uint32_t hi, const FlagUse &use, bool donor_exact = false) {
    Upper u;
    if ((hi & 0x3FFFFFFFu) == kVnopUpperWithoutFlags) return u;  // VNOP, known no-op encoding
    const unsigned dest = (hi >> 21) & 15u, ft = (hi >> 16) & 31u, fs = (hi >> 11) & 31u, fd = (hi >> 6) & 31u, bc = hi & 3u;
    const unsigned funct = hi & 63u;
    enum Kind { Nop, Add, Sub, Madd, Msub, Max, Min, Mul, Abs, Ftoi, Itof, Clip, Opmula, Opmsub } kind = Nop;
    enum Src { Vec, Bc, Q, I } src = Vec;
    int target = -1;
    bool flags = false;
    int frac = 0;
    static const int kFrac[4] = {0, 4, 12, 15};
    const auto set = [&](Kind k, Src s, int t, bool f) {
        kind = k;
        src = s;
        target = t;
        flags = f;
    };
    if (funct < 0x3C) {
        switch (funct) {
        case 0x00: case 0x01: case 0x02: case 0x03: set(Add, Bc, fd, true); break;
        case 0x04: case 0x05: case 0x06: case 0x07: set(Sub, Bc, fd, true); break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: set(Madd, Bc, fd, true); break;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(Msub, Bc, fd, true); break;
        case 0x10: case 0x11: case 0x12: case 0x13: set(Max, Bc, fd, false); break;
        case 0x14: case 0x15: case 0x16: case 0x17: set(Min, Bc, fd, false); break;
        case 0x18: case 0x19: case 0x1A: case 0x1B: set(Mul, Bc, fd, true); break;
        case 0x1C: set(Mul, Q, fd, true); break;
        case 0x1D: set(Max, I, fd, false); break;
        case 0x1E: set(Mul, I, fd, true); break;
        case 0x1F: set(Min, I, fd, false); break;
        case 0x20: set(Add, Q, fd, true); break;
        case 0x21: set(Madd, Q, fd, true); break;
        case 0x22: set(Add, I, fd, true); break;
        case 0x23: set(Madd, I, fd, true); break;
        case 0x24: set(Sub, Q, fd, true); break;
        case 0x25: set(Msub, Q, fd, true); break;
        case 0x26: set(Sub, I, fd, true); break;
        case 0x27: set(Msub, I, fd, true); break;
        case 0x28: set(Add, Vec, fd, true); break;
        case 0x29: set(Madd, Vec, fd, true); break;
        case 0x2A: set(Mul, Vec, fd, true); break;
        case 0x2B: set(Max, Vec, fd, false); break;
        case 0x2C: set(Sub, Vec, fd, true); break;
        case 0x2D: set(Msub, Vec, fd, true); break;
        case 0x2E: set(Opmsub, Vec, fd, true); break;
        case 0x2F: set(Min, Vec, fd, false); break;
        default: u.known = false; break;
        }
    } else {
        const unsigned index = ((hi >> 6) & 31u) * 4u + (funct & 3u);
        switch (index) {
        case 0x00: case 0x01: case 0x02: case 0x03: set(Add, Bc, kAcc, true); break;
        case 0x04: case 0x05: case 0x06: case 0x07: set(Sub, Bc, kAcc, true); break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: set(Madd, Bc, kAcc, true); break;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(Msub, Bc, kAcc, true); break;
        case 0x10: case 0x11: case 0x12: case 0x13: set(Itof, Vec, static_cast<int>(ft), false); frac = kFrac[index & 3u]; break;
        case 0x14: case 0x15: case 0x16: case 0x17: set(Ftoi, Vec, static_cast<int>(ft), false); frac = kFrac[index & 3u]; break;
        case 0x18: case 0x19: case 0x1A: case 0x1B: set(Mul, Bc, kAcc, true); break;
        case 0x1C: set(Mul, Q, kAcc, true); break;
        case 0x1D: set(Abs, Vec, static_cast<int>(ft), false); break;
        case 0x1E: set(Mul, I, kAcc, true); break;
        case 0x1F: set(Clip, Vec, -1, false); break;
        case 0x20: set(Add, Q, kAcc, true); break;
        case 0x21: set(Madd, Q, kAcc, true); break;
        case 0x22: set(Add, I, kAcc, true); break;
        case 0x23: set(Madd, I, kAcc, true); break;
        case 0x24: set(Sub, Q, kAcc, true); break;
        case 0x25: set(Msub, Q, kAcc, true); break;
        case 0x26: set(Sub, I, kAcc, true); break;
        case 0x27: set(Msub, I, kAcc, true); break;
        case 0x28: set(Add, Vec, kAcc, true); break;
        case 0x29: set(Madd, Vec, kAcc, true); break;
        case 0x2A: set(Mul, Vec, kAcc, true); break;
        case 0x2C: set(Sub, Vec, kAcc, true); break;
        case 0x2D: set(Msub, Vec, kAcc, true); break;
        case 0x2E: set(Opmula, Vec, kAcc, true); break;
        default: u.known = false; break;
        }
    }
    if (kind == Nop) return u;
    if (kind == Clip) {
        // The clip word is a chain through every CLIP instruction (clip_result), so the
        // value is always computed; only the FlagPipe push depends on a clip reader.
        u.clip = true;
        u.pre = std::format("const std::uint32_t uclip = clip_result(last_clip, r.vf[{}], r.vf[{}].w[3]); ", fs, ft);
        u.post = "last_clip = uclip; ";
        if (use.clip_reader) u.post += "fl.set_clip(uclip, fl.t); ";
        return u;
    }
    unsigned d = dest;
    if (kind == Opmula || kind == Opmsub) d = 0xE;
    if (d == 0) {
        // No lane is written, but the donor interpreter still runs set_flags on the empty
        // result, which sets MAC to 0 and pushes it. A flag-setting op is therefore a MAC
        // writer here as well.
        if (flags) {
            u.sets_flags = true;
            u.post = mac_update("r.vf[0]", 0, use);
        }
        return u;
    }
    const bool mac_kept = flags && (use.full || use.mac_reader || use.mac_exit);
    if (target == 0 && !mac_kept) return u;
    const auto b = [&](int i) -> std::string {
        switch (src) {
        case Vec: return vf(ft, i);
        case Bc: return vf(ft, static_cast<int>(bc));
        case Q: return "r.q";
        default: return "r.i";
        }
    };
    const std::string dst = target == kAcc ? "r.acc" : std::format("r.vf[{}]", target);
    const bool vector = !donor_exact && (kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub || kind == Max ||
                                         kind == Min || kind == Abs || kind == Ftoi || kind == Itof);
    const bool arithmetic = kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub;
    if (target != 0) {
        u.target = target;
        u.mask = d;
        u.normalized = vector && arithmetic;
    }
    if ((kind == Opmula || kind == Opmsub) && !donor_exact) {
        // TASK-134: the outer product over x, y and z at once, fs.yzx times ft.zxy (lane w is computed, never written). Each
        // lane equals the scalar mul and msub below: vmul and vmsub fix their operands and their result as fin and fout do.
        const std::string fs_yzx = std::format("__builtin_shufflevector({0}, {0}, 1, 2, 0, 3)", vreg(fs));
        const std::string ft_zxy = std::format("__builtin_shufflevector({0}, {0}, 2, 0, 1, 3)", vreg(ft));
        if (kind == Opmula) u.pre += std::format("const v4u uv = vmul({}, {}); ", fs_yzx, ft_zxy);
        else u.pre += std::format("const v4u uv = vmsub(vload(r.acc), {}, {}); ", fs_yzx, ft_zxy);
        if (target == 0) {
            const std::string mac = mac_update("uf", d, use);
            if (mac.empty()) u.pre.clear();  // VF0 is not written and no MAC is observed: nothing to compute
            else u.post = std::format("{{ Reg128 uf; vstore(uf, uv); {}}} ", mac);
            u.sets_flags = true;
            return u;
        }
        u.normalized = true;
        u.post += std::format("vstore({0}, vsel(vload({0}), uv, {1})); ", dst, lane_mask(d));
        u.sets_flags = flags;
        if (flags) u.post += mac_update(dst, d, use);
        return u;
    }
    if (arithmetic && !donor_exact) {
        const auto operand = [&](unsigned reg) -> std::string {
            if (!reg) return vreg(0);
            u.shadow_reads.push_back(static_cast<int>(reg));
            return shadow(static_cast<int>(reg));
        };
        std::string nb;
        switch (src) {
        case Vec: nb = operand(ft); break;
        case Bc:
            if (ft) u.shadow_reads.push_back(static_cast<int>(ft));
            nb = ft ? std::format("vsplat(n{}[{}])", ft, bc) : std::format("vsplat({})", vf(0, static_cast<int>(bc)));
            break;
        case Q: nb = "vfix(vsplat(r.q))"; break;
        default: nb = "vfix(vsplat(r.i))"; break;
        }
        const std::string ns = operand(fs);
        if (kind == Madd || kind == Msub) u.shadow_reads.push_back(kAcc);
        // TASK-134: a product with VF0.w (1.0) is the other operand itself: a fixed value (normal, signed zero or at most
        // 0x7F7FFFFF) times 1.0f is exact and vout leaves it as it is. MULbc, MADDbc and MSUBbc by vf0w (the w row of a
        // vertex transform) become a copy, an add and a subtract with the same bits.
        const bool times_one = src == Bc && ft == 0 && bc == 3;
        switch (kind) {
        case Add: u.pre += std::format("const v4u uv = vaddn({}, {}); ", ns, nb); break;
        case Sub: u.pre += std::format("const v4u uv = vsubn({}, {}); ", ns, nb); break;
        case Mul:
            if (times_one) u.pre += std::format("const v4u uv = {}; ", ns);
            else u.pre += std::format("const v4u uv = vmuln({}, {}); ", ns, nb);
            break;
        case Madd:
            if (times_one) u.pre += std::format("const v4u uv = vaddn(nacc, {}); ", ns);
            else u.pre += std::format("const v4u uv = vmaddn(nacc, {}, {}); ", ns, nb);
            break;
        default:
            if (times_one) u.pre += std::format("const v4u uv = vsubn(nacc, {}); ", ns);
            else u.pre += std::format("const v4u uv = vmsubn(nacc, {}, {}); ", ns, nb);
            break;
        }
    }
    if (vector) {
        std::string vb;
        switch (src) {
        case Vec: vb = vreg(ft); break;
        case Bc: vb = std::format("vsplat({})", vf(ft, static_cast<int>(bc))); break;
        case Q: vb = "vsplat(r.q)"; break;
        default: vb = "vsplat(r.i)"; break;
        }
        const std::string vs = vreg(fs);
        std::string e;
        switch (kind) {
        case Max: e = std::format("vmax({}, {})", vs, vb); break;
        case Min: e = std::format("vmin({}, {})", vs, vb); break;
        case Abs: e = std::format("vabs({})", vs); break;
        case Ftoi: e = std::format("vftoi({}, {})", vs, frac); break;
        case Itof: e = std::format("vitof({}, {})", vs, frac); break;
        default: break;
        }
        if (!e.empty()) u.pre += std::format("const v4u uv = {}; ", e);
        if (target == 0) {
            const std::string mac = mac_update("uf", d, use);
            if (mac.empty()) u.pre.clear();  // VF0 is not written and no MAC is observed: nothing to compute
            else u.post = std::format("{{ Reg128 uf; vstore(uf, uv); {}}} ", mac);
            u.sets_flags = true;
            return u;
        }
        if (d == 0xF) u.post += std::format("vstore({}, uv); ", dst);
        else u.post += std::format("vstore({0}, vsel(vload({0}), uv, {1})); ", dst, lane_mask(d));
        u.sets_flags = flags;
        if (flags) u.post += mac_update(dst, d, use);
        return u;
    }
    if (donor_exact && arithmetic && std::popcount(d) >= 2 && fs != 0 && !(src == Vec && ft == 0)) {
        // TASK-134: VU0 (exact): the lanes in one fp4 operation (native/fp4.hpp, the bits of fp:: per lane, native_vu_fast_ops).
        // A single lane stays scalar, and so does VF0 as a source register (its lanes are constants, not read from r.vf[0]).
        std::string b4;
        switch (src) {
        case Vec: b4 = std::format("fp4::load(r.vf[{}])", ft); break;
        case Bc: b4 = std::format("fp4::splat({})", vf(ft, static_cast<int>(bc))); break;
        case Q: b4 = "fp4::splat(r.q)"; break;
        default: b4 = "fp4::splat(r.i)"; break;
        }
        const std::string a4 = std::format("fp4::load(r.vf[{}])", fs);
        std::string e;
        switch (kind) {
        case Add: e = std::format("fp4::add({}, {})", a4, b4); break;
        case Sub: e = std::format("fp4::sub({}, {})", a4, b4); break;
        case Mul: e = std::format("fp4::mul({}, {})", a4, b4); break;
        case Madd: e = std::format("fp4::add(fp4::load(r.acc), fp4::mul({}, {}))", a4, b4); break;
        default: e = std::format("fp4::sub(fp4::load(r.acc), fp4::mul({}, {}))", a4, b4); break;
        }
        u.pre += std::format("const fp4::V u4 = {}; ", e);
        if (target == 0) {
            const std::string mac = mac_update("uf", d, use);
            if (mac.empty()) u.pre.clear();  // VF0 is not written and no MAC is observed: nothing to compute
            else u.post = std::format("{{ Reg128 uf{{}}; fp4::store_lanes(uf, {}u, u4); {}}} ", d, mac);
            u.sets_flags = true;
            return u;
        }
        u.post += std::format("fp4::store_lanes({}, {}u, u4); ", dst, d);
        u.sets_flags = flags;
        if (flags) u.post += mac_update(dst, d, use);
        return u;
    }
    const char *fp_prefix = donor_exact ? "fp::" : "";
    for (int i = 0; i < 4; ++i) {
        if (!lane(d, i)) continue;
        std::string e;
        switch (kind) {
        case Add: e = std::format("{}add({}, {})", fp_prefix, vf(fs, i), b(i)); break;
        case Sub: e = std::format("{}sub({}, {})", fp_prefix, vf(fs, i), b(i)); break;
        case Mul: e = std::format("{}mul({}, {})", fp_prefix, vf(fs, i), b(i)); break;
        case Madd:
            e = donor_exact ? std::format("fp::add(r.acc.w[{}], fp::mul({}, {}))", i, vf(fs, i), b(i)) : std::format("madd(r.acc.w[{}], {}, {})", i, vf(fs, i), b(i));
            break;
        case Msub:
            e = donor_exact ? std::format("fp::sub(r.acc.w[{}], fp::mul({}, {}))", i, vf(fs, i), b(i)) : std::format("msub(r.acc.w[{}], {}, {})", i, vf(fs, i), b(i));
            break;
        case Max: e = std::format("max({}, {})", vf(fs, i), b(i)); break;
        case Min: e = std::format("min({}, {})", vf(fs, i), b(i)); break;
        case Abs: e = std::format("({} & 0x7FFFFFFFu)", vf(fs, i)); break;
        case Ftoi: e = std::format("fp::to_fixed({}, {})", vf(fs, i), frac); break;
        case Itof: e = std::format("fp::from_fixed(r.vf[{}].sw[{}], {})", fs, i, frac); break;
        case Opmula: {
            static const int a[3] = {1, 2, 0}, c[3] = {2, 0, 1};
            e = donor_exact ? std::format("fp::mul({}, {})", vf(fs, a[i]), vf(ft, c[i])) : std::format("mul({}, {})", vf(fs, a[i]), vf(ft, c[i]));
            break;
        }
        case Opmsub: {
            static const int a[3] = {1, 2, 0}, c[3] = {2, 0, 1};
            e = donor_exact ? std::format("fp::sub(r.acc.w[{}], fp::mul({}, {}))", i, vf(fs, a[i]), vf(ft, c[i]))
                            : std::format("msub(r.acc.w[{}], {}, {})", i, vf(fs, a[i]), vf(ft, c[i]));
            break;
        }
        default: break;
        }
        u.pre += std::format("const std::uint32_t u{} = {}; ", kLane[i], e);
    }
    if (target == 0) {
        const std::string mac = mac_update("uf", d, use);
        if (mac.empty()) {
            u.pre.clear();  // VF0 is not written and no MAC is observed: nothing to compute
        } else {
            u.post = "{ Reg128 uf{}; ";
            for (int i = 0; i < 4; ++i)
                if (lane(d, i)) u.post += std::format("uf.w[{}] = u{}; ", i, kLane[i]);
            u.post += mac + "} ";
        }
        u.sets_flags = true;
        return u;
    }
    for (int i = 0; i < 4; ++i)
        if (lane(d, i)) u.post += std::format("{}.w[{}] = u{}; ", dst, i, kLane[i]);
    u.sets_flags = flags;
    if (flags) u.post += mac_update(dst, d, use);
    return u;
}

// TASK-134: a partial destination is written as a whole qword with the other lanes kept (vsel), not lane by lane. The register (or
// memory qword) is read back as 16 bytes soon after, and a 16-byte read of four separate 4-byte stores cannot be forwarded from the
// store buffer: the processor waits for the stores to reach the cache. The bytes written are the same. A qword is always inside
// VU memory (qword() masks the index).
std::string lanes_copy(unsigned dest, const std::string &to, const std::string &from_qword) {
    if (dest == 0xF) return std::format("std::memcpy(&{}, {}, 16); ", to, from_qword);
    return std::format("vstore({0}, vsel(vload({0}), vload_bytes({1}), {2})); ", to, from_qword, lane_mask(dest));
}

std::string lanes_store(unsigned dest, const std::string &qword_expr, const std::string &from) {
    if (dest == 0xF) return std::format("std::memcpy({}, &{}, 16); ", qword_expr, from);
    return std::format("vstore_bytes({0}, vsel(vload_bytes({0}), vload({1}), {2})); ", qword_expr, from, lane_mask(dest));
}

struct Lower {
    std::string code;
    int writes = -1;
    bool reads_flags = false, reads_status = false, reads_clip = false;
    bool branch = false, jump = false;
    bool kick = false;  // XGKICK (refused for VU0)
    std::uint32_t target = 0;
    bool supported = true;
};

Lower lower(std::uint32_t w, std::uint32_t pc, std::uint32_t mask) {
    Lower l;
    const unsigned dest = (w >> 21) & 15u, it = (w >> 16) & 15u, is = (w >> 11) & 15u, id = (w >> 6) & 15u;
    const unsigned ft = (w >> 16) & 31u, fs = (w >> 11) & 31u;
    const std::int32_t imm11 = static_cast<std::int32_t>(w << 21) >> 21;
    std::string &c = l.code;
    if ((w >> 25) == 0x40u) {
        const unsigned funct = w & 63u;
        if (funct < 0x3C) {
            switch (funct) {
            case 0x30: c = std::format("set_vi(r, {}, r.vi[{}] + r.vi[{}]); ", id, is, it); break;
            case 0x31: c = std::format("set_vi(r, {}, r.vi[{}] - r.vi[{}]); ", id, is, it); break;
            case 0x32: c = std::format("set_vi(r, {}, r.vi[{}] + {}u); ", it, is, static_cast<std::uint32_t>(static_cast<std::int32_t>(((w >> 6) & 31u) << 27) >> 27)); break;
            case 0x34: c = std::format("set_vi(r, {}, r.vi[{}] & r.vi[{}]); ", id, is, it); break;
            case 0x35: c = std::format("set_vi(r, {}, r.vi[{}] | r.vi[{}]); ", id, is, it); break;
            default: l.supported = false; break;
            }
            return l;
        }
        const unsigned index = ((w >> 6) & 31u) * 4u + (funct & 3u);
        const unsigned fsf = (w >> 21) & 3u, ftf = (w >> 23) & 3u;
        switch (index) {
        case 0x30:
            if (ft) l.writes = static_cast<int>(ft);
            // TASK-134: the lanes as one qword (see lanes_copy).
            if (ft && dest) c += std::format("vstore(r.vf[{0}], vsel(vload(r.vf[{0}]), vload(r.vf[{1}]), {2})); ", ft, fs, lane_mask(dest));
            break;
        case 0x31:
            if (ft) l.writes = static_cast<int>(ft);
            if (ft && dest) {
                // MR32: lane i takes fs lane i + 1 (w takes x), as one qword.
                c = std::format("{{ const v4u m = vload(r.vf[{0}]); vstore(r.vf[{1}], vsel(vload(r.vf[{1}]), __builtin_shufflevector(m, m, 1, 2, 3, 0), {2})); }} ",
                                fs, ft, lane_mask(dest));
            }
            break;
        case 0x34:
            if (ft) l.writes = static_cast<int>(ft);
            c = std::format("{{ const std::uint8_t *q = qword(mem, r.vi[{}]); ", is);
            if (ft) c += lanes_copy(dest, std::format("r.vf[{}]", ft), "q");
            c += std::format("set_vi(r, {}, r.vi[{}] + 1u); }} ", is, is);
            break;
        case 0x35:
            c = std::format("{{ std::uint8_t *q = qword(mem, r.vi[{}]); ", it) + lanes_store(dest, "q", std::format("r.vf[{}]", fs)) +
                std::format("set_vi(r, {}, r.vi[{}] + 1u); }} ", it, it);
            break;
        case 0x36:
            if (ft) l.writes = static_cast<int>(ft);
            c = std::format("{{ set_vi(r, {}, r.vi[{}] - 1u); const std::uint8_t *q = qword(mem, r.vi[{}]); ", is, is, is);
            if (ft) c += lanes_copy(dest, std::format("r.vf[{}]", ft), "q");
            c += "} ";
            break;
        case 0x37:
            c = std::format("{{ set_vi(r, {}, r.vi[{}] - 1u); std::uint8_t *q = qword(mem, r.vi[{}]); ", it, it, it) +
                lanes_store(dest, "q", std::format("r.vf[{}]", fs)) + "} ";
            break;
        case 0x38: c = std::format("vu::div(r, r.vf[{}].w[{}], r.vf[{}].w[{}]); ", fs, fsf, ft, ftf); break;
        case 0x39: c = std::format("vu::sqrt(r, r.vf[{}].w[{}]); ", ft, ftf); break;
        case 0x3A: c = std::format("vu::rsqrt(r, r.vf[{}].w[{}], r.vf[{}].w[{}]); ", fs, fsf, ft, ftf); break;
        case 0x3B: break;
        case 0x3C: c = std::format("set_vi(r, {}, r.vf[{}].w[{}]); ", it, fs, fsf); break;
        case 0x3D:
            if (ft) l.writes = static_cast<int>(ft);
            if (ft) {
                c = std::format("{{ const std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(r.vi[{}]))); ", is);
                for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("r.vf[{}].w[{}] = v; ", ft, i);
                c += "} ";
            }
            break;
        case 0x3E:
            for (int i = 0; i < 4; ++i)
                if (lane(dest, i)) {
                    c = std::format("{{ std::uint16_t v; std::memcpy(&v, qword(mem, r.vi[{}]) + {}, 2); set_vi(r, {}, v); }} ", is, i * 4, it);
                    break;
                }
            break;
        case 0x3F:
            c = std::format("{{ std::uint8_t *q = qword(mem, r.vi[{}]); const std::uint32_t v = r.vi[{}]; ", is, it);
            for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("std::memcpy(q + {}, &v, 4); ", i * 4);
            c += "} ";
            break;
        case 0x40:
            if (ft) l.writes = static_cast<int>(ft);
            c = "r.r = vu::rand_next(r.r); ";
            if (ft) for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("r.vf[{}].w[{}] = r.r | fp::kOne; ", ft, i);
            break;
        case 0x41:
            if (ft) l.writes = static_cast<int>(ft);
            if (ft) for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("r.vf[{}].w[{}] = (r.r & 0x7FFFFFu) | fp::kOne; ", ft, i);
            break;
        case 0x42: c = std::format("r.r = r.vf[{}].w[{}] & 0x7FFFFFu; ", fs, fsf); break;
        case 0x43: c = std::format("r.r = (r.r ^ r.vf[{}].w[{}]) & 0x7FFFFFu; ", fs, fsf); break;
        case 0x64:
            if (ft) l.writes = static_cast<int>(ft);
            if (ft) for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("r.vf[{}].w[{}] = r.p; ", ft, i); break;
        case 0x68: c = std::format("set_vi(r, {}, vu::top()); ", it); break;
        case 0x69: c = std::format("set_vi(r, {}, vu::itop()); ", it); break;
        case 0x6C: c = std::format("vu::kick(1, r.vi[{}]); ", is); l.kick = true; break;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x78: case 0x79: case 0x7A: case 0x7C: case 0x7D: case 0x7E:
            c = std::format("efu(r, {}u, {}u, {}u); ", index, fs, fsf);
            break;
        case 0x7B: break;
        default: l.supported = false; break;
        }
        return l;
    }
    const std::uint32_t imm15 = ((w >> 10) & 0x7800u) | (w & 0x7FFu);
    const std::uint32_t imm12 = ((w >> 10) & 0x800u) | (w & 0x7FFu);
    const std::uint32_t target = (pc + 8u + static_cast<std::uint32_t>(imm11) * 8u) & mask;
    switch (w >> 25) {
    case 0x00:
        if (ft) l.writes = static_cast<int>(ft);
        c = std::format("{{ const std::uint8_t *q = qword(mem, r.vi[{}] + {}u); ", is, static_cast<std::uint32_t>(imm11));
        if (ft) c += lanes_copy(dest, std::format("r.vf[{}]", ft), "q");
        c += "} ";
        break;
    case 0x01:
        c = std::format("{{ std::uint8_t *q = qword(mem, r.vi[{}] + {}u); ", it, static_cast<std::uint32_t>(imm11)) + lanes_store(dest, "q", std::format("r.vf[{}]", fs)) + "} ";
        break;
    case 0x04:
        for (int i = 0; i < 4; ++i)
            if (lane(dest, i)) {
                c = std::format("{{ std::uint16_t v; std::memcpy(&v, qword(mem, r.vi[{}] + {}u) + {}, 2); set_vi(r, {}, v); }} ", is, static_cast<std::uint32_t>(imm11), i * 4, it);
                break;
            }
        break;
    case 0x05:
        c = std::format("{{ std::uint8_t *q = qword(mem, r.vi[{}] + {}u); const std::uint32_t v = r.vi[{}]; ", is, static_cast<std::uint32_t>(imm11), it);
        for (int i = 0; i < 4; ++i) if (lane(dest, i)) c += std::format("std::memcpy(q + {}, &v, 4); ", i * 4);
        c += "} ";
        break;
    case 0x08: c = std::format("set_vi(r, {}, r.vi[{}] + {}u); ", it, is, imm15); break;
    case 0x09: c = std::format("set_vi(r, {}, r.vi[{}] - {}u); ", it, is, imm15); break;
    case 0x10: c = std::format("set_vi(r, 1, (fl.clip() & 0xFFFFFFu) == {}u ? 1u : 0u); ", w & 0xFFFFFFu); l.reads_clip = true; break;
    case 0x11: c = std::format("last_clip = {}u; fl.set_clip(last_clip, fl.t - 4u); ", w & 0xFFFFFFu); break;
    case 0x12: c = std::format("set_vi(r, 1, (fl.clip() & {}u) ? 1u : 0u); ", w & 0xFFFFFFu); l.reads_clip = true; break;
    case 0x13: c = std::format("set_vi(r, 1, ((fl.clip() | {}u) & 0xFFFFFFu) == 0xFFFFFFu ? 1u : 0u); ", w & 0xFFFFFFu); l.reads_clip = true; break;
    case 0x14: c = std::format("set_vi(r, {}, (fl.status() & 0xFFFu) == {}u ? 1u : 0u); ", it, imm12); l.reads_flags = l.reads_status = true; break;
    case 0x15: c = std::format("r.status = (fl.status() & 0x3Fu) | ({}u & 0xFC0u); fl.force_mac(r.mac, r.status); ", imm12); l.reads_flags = l.reads_status = true; break;
    case 0x16: c = std::format("set_vi(r, {}, fl.status() & {}u); ", it, imm12); l.reads_flags = l.reads_status = true; break;
    case 0x17: c = std::format("set_vi(r, {}, ((fl.status() | {}u) & 0xFFFu) == 0xFFFu ? 1u : 0u); ", it, imm12); l.reads_flags = l.reads_status = true; break;
    case 0x18: c = std::format("set_vi(r, {}, (fl.mac() & 0xFFFFu) == r.vi[{}] ? 1u : 0u); ", it, is); l.reads_flags = true; break;
    case 0x1A: c = std::format("set_vi(r, {}, fl.mac() & r.vi[{}]); ", it, is); l.reads_flags = true; break;
    case 0x1B: c = std::format("set_vi(r, {}, ((fl.mac() | r.vi[{}]) & 0xFFFFu) == 0xFFFFu ? 1u : 0u); ", it, is); l.reads_flags = true; break;
    case 0x1C: c = std::format("set_vi(r, {}, fl.clip() & 0xFFFu); ", it); l.reads_clip = true; break;
    case 0x20: c = "pend = true; "; l.branch = true; l.target = target; break;
    case 0x21: c = std::format("set_vi(r, {}, {}u); pend = true; ", it, (pc + 16u) / 8u); l.branch = true; l.target = target; break;
    case 0x24: c = std::format("jpc = (static_cast<std::uint32_t>(r.vi[{}]) * 8u) & {}u; pendj = true; ", is, mask); l.jump = true; break;
    case 0x25: c = std::format("jpc = (static_cast<std::uint32_t>(r.vi[{}]) * 8u) & {}u; set_vi(r, {}, {}u); pendj = true; ", is, mask, it, (pc + 16u) / 8u); l.jump = true; break;
    case 0x28: c = std::format("pend = r.vi[{}] == r.vi[{}]; ", is, it); l.branch = true; l.target = target; break;
    case 0x29: c = std::format("pend = r.vi[{}] != r.vi[{}]; ", is, it); l.branch = true; l.target = target; break;
    case 0x2C: c = std::format("pend = static_cast<std::int16_t>(r.vi[{}]) < 0; ", is); l.branch = true; l.target = target; break;
    case 0x2D: c = std::format("pend = static_cast<std::int16_t>(r.vi[{}]) > 0; ", is); l.branch = true; l.target = target; break;
    case 0x2E: c = std::format("pend = static_cast<std::int16_t>(r.vi[{}]) <= 0; ", is); l.branch = true; l.target = target; break;
    case 0x2F: c = std::format("pend = static_cast<std::int16_t>(r.vi[{}]) >= 0; ", is); l.branch = true; l.target = target; break;
    default: l.supported = false; break;
    }
    return l;
}



void replace_all(std::string &s, const std::string &from, const std::string &to) {
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

// Keeps VI registers in locals inside one function. Locals are written back at
// each VUGEN_SYNC; marker before interpreter calls, exits and budget exhaustion.
// extra_sync writes the other exit-state locals (MAC, clip) back at the same points.
std::string localize_vi(std::string o, const std::string &extra_sync) {
    std::string declarations, sync;
    replace_all(o, "set_vi(r, 0, ", "drop_vi(");
    replace_all(o, "r.vi[0]", "0u");
    for (int n = 1; n < 16; ++n) {
        const std::string set = std::format("set_vi(r, {}, ", n), get = std::format("r.vi[{}]", n);
        const bool written = o.find(set) != std::string::npos;
        if (!written && o.find(get) == std::string::npos) continue;
        replace_all(o, set, std::format("set_vi(i{}, ", n));
        replace_all(o, get, std::format("i{}", n));
        declarations += std::format("    std::uint16_t i{} = r.vi[{}];\n", n, n);
        if (written) sync += std::format("r.vi[{}] = i{}; ", n, n);
    }
    replace_all(o, "    VUGEN_LOCALS;\n", declarations);
    replace_all(o, "VUGEN_SYNC;", sync + extra_sync);
    return o;
}

// Observation windows, in instructions. FlagPipe::mac_slot reads the newest MAC
// entry with mac_ts + 4 <= t, and clip() the newest clip entry with clip_ts + 3 <= t,
// so a write at distance k from a reader is seen only if no later write of the same
// channel sits at distance 1 .. d - window.
constexpr int kMacWindow = 4;
constexpr int kClipWindow = 3;
// Observation states per node: none (no later write yet) and 0 .. window - 1.
constexpr int kStateSlots = kMacWindow + 1;

// How MAC and status are kept for one program.
enum class MacMode {
    None,      // the image has no MAC reader: the donor interpreter keeps no flags either
    Full,      // donor behaviour: flags() with status and a FlagPipe entry for every write
    Observed,  // MAC only where a reader or the program exit can see it; status is not kept
};

// Whether a write can be seen by a later reader of its channel, or is the last write
// before some exit of the program (exit state is exact, see generate()).
struct Observed {
    bool reader = false;
    bool exit = false;
};

// Per-program analysis and text generation. Donor delay-slot model: a node
// is followed by pc+8, and a delay-slot node is also followed by the target of
// its branch. A successor of -1 marks an exit.
class FunctionGenerator {
public:
    explicit FunctionGenerator(ProgramRecord &record) : record_(record), code_(record.image.data()), profile_(unit_profile(record.unit)), mask_(profile_.code_mask()) {}

    std::string run() {
        pcs_ = ps2::vu::reachable(code_, mask_, record_.entry);
        in_.insert(pcs_.begin(), pcs_.end());
        analyze();
        record_.fallback_sites = fallback_count_;
        record_.upper_nop_sites = upper_nop_count_;
        record_.exit_pcs = exit_pcs_;
        if (!refusal_.empty()) {
            record_.refused = true;
            record_.refusal = refusal_;
            record_.function_name.clear();
            return {};
        }
        record_.function_name = std::format("{}{:016X}", profile_.function_prefix, record_.key);
        std::string text = generate();
        // Every interpreter hand-off (lower exit, unknown successor, dispatch, end) is a call here.
        const std::string handoff_token = profile_.unit == 1 ? "ps2::vu::interpret(" : "ctx.exact_run(";
        for (std::size_t at = text.find(handoff_token); at != std::string::npos; at = text.find(handoff_token, at + 1)) ++record_.interpreter_calls;
        return text;
    }

private:
    ProgramRecord &record_;
    const std::uint8_t *code_;
    const UnitProfile &profile_;
    const std::uint32_t mask_;
    std::vector<std::uint32_t> pcs_;
    std::set<std::uint32_t> dispatch_targets_;  // VU0: static return sites of jump-register dispatch
    std::set<std::uint32_t> in_;
    std::map<std::uint32_t, Lower> lowers_;
    std::map<std::uint32_t, Upper> uppers_;
    std::map<std::uint32_t, FlagUse> use_;
    MacMode mac_mode_ = MacMode::Full;
    bool clip_all_live_ = false;
    // Visit marks for observe(): one slot per (node, state), compared with stamp_.
    std::vector<std::uint32_t> stamps_;
    std::uint32_t stamp_ = 0;
    std::set<int> shadowed_;
    std::size_t fallback_count_ = 0;
    std::size_t upper_nop_count_ = 0;
    std::vector<std::uint32_t> exit_pcs_;
    std::string refusal_;
    bool uses_dispatch_ = false, uses_exhausted_ = false;

    std::uint32_t lo(std::uint32_t pc) const { return word(record_.image, pc); }
    std::uint32_t hi(std::uint32_t pc) const { return word(record_.image, pc + 4); }

    std::vector<std::int64_t> successors(std::uint32_t pc) const {
        std::vector<std::int64_t> out{static_cast<std::int64_t>((pc + 8u) & mask_)};
        const std::uint32_t prev = (pc - 8u) & mask_;
        if (in_.count(prev)) {
            const Lower &previous = lowers_.at(prev);
            const std::uint32_t previous_hi = hi(prev);
            if (previous.branch) out.push_back(previous.target);
            if (previous.jump || (previous_hi & kEndBit)) out.push_back(-1);
        }
        return out;
    }

    // Only lower encodings the donor emitter does not model exit to the interpreter.
    // Unknown upper encodings are no-ops in the donor interpreter (fast_upper default)
    // and are emitted as such; they are counted in upper_nop_sites.
    bool lower_exit(std::uint32_t pc) const {
        const bool immediate = hi(pc) & kImmediateBit;
        return !immediate && !lowers_.at(pc).supported;
    }

    // An unknown or unsupported instruction exits to the interpreter. That exit
    // cannot carry a pending delay branch or an end-of-program bit from the
    // previous instruction, so such programs are refused, not emitted.
    bool fallback_hazard(std::uint32_t pc) const {
        const std::uint32_t prev = (pc - 8u) & mask_;
        if (!in_.count(prev)) return false;
        return lowers_.at(prev).branch || lowers_.at(prev).jump || (hi(prev) & kEndBit);
    }

    void analyze() {
        stamps_.assign(static_cast<std::size_t>(profile_.image_bytes) / 8u * kStateSlots, 0u);
        // Pass 1 decodes every instruction so later passes can look at neighbours.
        for (std::uint32_t pc : pcs_) {
            const std::uint32_t lower_word = lo(pc), upper_word = hi(pc);
            Lower lower_info;
            if (!(upper_word & kImmediateBit)) lower_info = lower(lower_word, pc, mask_);
            lowers_[pc] = lower_info;
            uppers_[pc] = upper(upper_word, FlagUse{}, profile_.unit == 0);
            // A lower clip write (0x11) is visible at once and is not modelled by the
            // window analysis, so a program that has one keeps every clip write.
            if (!(upper_word & kImmediateBit) && (lower_word >> 25) == 0x11u) clip_all_live_ = true;
        }
        // Pass 2 counts interpreter exits and refuses hazardous ones.
        for (std::uint32_t pc : pcs_) {
            if (!uppers_.at(pc).known) ++upper_nop_count_;
            // VU0 (exact policy): code past the EE upload is not in the uploaded image, and XGKICK is not
            // supported (its DMA chain would bypass the exact hand-off). Both refusals are recorded in the manifest.
            if (refusal_.empty() && pc >= profile_.uploaded_bytes)
                refusal_ = std::format("reachable instruction at 0x{:04X} is beyond the 0x{:04X} uploaded code", pc, profile_.uploaded_bytes);
            if (refusal_.empty() && !profile_.xgkick_allowed && !(hi(pc) & kImmediateBit) && lowers_.at(pc).kick)
                refusal_ = std::format("XGKICK at 0x{:04X} is not supported for unit {}", pc, profile_.unit);
            if (!lower_exit(pc)) continue;
            ++fallback_count_;
            exit_pcs_.push_back(pc);
            if (refusal_.empty() && fallback_hazard(pc))
                refusal_ = std::format("interpreter exit at 0x{:04X} follows a branch or end bit", pc);
        }
        // An end bit on a branch or jump delay slot: the donor runs the branch target once more
        // and then ends, while the emitted code would continue past the end. Refused, not emitted.
        for (std::uint32_t pc : pcs_) {
            const std::uint32_t prev = (pc - 8u) & mask_;
            if (!(hi(pc) & kEndBit) || !in_.count(prev) || !refusal_.empty()) continue;
            if (lowers_.at(prev).branch || lowers_.at(prev).jump)
                refusal_ = std::format("end bit at 0x{:04X} sits in the delay slot of a branch or jump", pc);
        }
        collect_dispatch_targets();
        select_flag_uses();
        for (std::uint32_t pc : pcs_)
            for (int reg : uppers_.at(pc).shadow_reads) shadowed_.insert(reg);
    }

    bool channel_reader(std::uint32_t pc, bool mac) const { return mac ? lowers_.at(pc).reads_flags : lowers_.at(pc).reads_clip; }
    bool channel_writer(std::uint32_t pc, bool mac) const { return mac ? uppers_.at(pc).sets_flags : uppers_.at(pc).clip; }

    // Decides, for every reachable instruction, which of its flag results must be
    // computed, and fills the site counters. Full keeps everything; Observed asks
    // observe() for each write.
    // Static return sites of jump-register dispatch (VU0). A link value (BAL or JALR) that lands on a
    // reachable instruction is a candidate; its predecessor must not be a branch, jump or end bit,
    // because the label is entered without executing that predecessor.
    void collect_dispatch_targets() {
        for (std::uint32_t pc : pcs_) {
            if (hi(pc) & kImmediateBit) continue;
            const std::uint32_t op = lo(pc) >> 25;
            if (op != 0x21 && op != 0x25) continue;
            const std::uint32_t target = (pc + 16u) & mask_;
            if (!in_.count(target)) continue;
            const std::uint32_t prev = (target - 8u) & mask_;
            if (in_.count(prev) && (lowers_.at(prev).branch || lowers_.at(prev).jump || (hi(prev) & kEndBit))) continue;
            dispatch_targets_.insert(target);
        }
    }

    // Exit to the interpreter for a pc expression: VU1 uses the donor interpreter (unchanged text), VU0 the
    // exact run(0, pc) hand-off from the runtime context.
    std::string exit_call(const std::string &pc_text) const {
        return profile_.unit == 1 ? "ps2::vu::interpret(1, " + pc_text + ")" : "ctx.exact_run(" + pc_text + ")";
    }

    // The jump-register dispatch. VU1 is unchanged. VU0 switches on the jump target: a static return site
    // continues compiled code (budget charged as for any entry), any other target takes the exact hand-off.
    std::string dispatch_text() {
        if (profile_.unit == 1) return "dispatch:\n    VUGEN_SYNC; ps2::vu::interpret(1, jpc); return RunStatus::Completed;\n";
        std::string o = "dispatch:\n    switch (jpc) {\n";
        for (std::uint32_t target : dispatch_targets_) {
            o += std::format("    case {}u:\n    ", target);
            o += charge(target);
            o += std::format("goto {};\n", label(target));
        }
        o += "    default: break;\n    }\n";
        o += "    VUGEN_SYNC; " + exit_call("jpc") + "; return RunStatus::Completed;\n";
        return o;
    }

    void select_flag_uses() {
        // VU0 keeps MAC, status and clip exact at every write (EE reads them with CFC2): full flags, no elision.
        if (profile_.exact_flags) mac_mode_ = MacMode::Full;
        else if (!record_.image_flags_used) mac_mode_ = MacMode::None;
        else if (flag_elision() && !record_.image_status_used) mac_mode_ = MacMode::Observed;
        else mac_mode_ = MacMode::Full;
        record_.mac_mode = mac_mode_ == MacMode::None ? "none" : mac_mode_ == MacMode::Full ? "full" : "observed";
        const bool clip_all = clip_all_live_ || !flag_elision() || profile_.exact_flags;
        for (std::uint32_t pc : pcs_) {
            const Upper &analysis = uppers_.at(pc);
            FlagUse use;
            if (mac_mode_ == MacMode::None) {
                use.full = use.mac_reader = use.mac_exit = false;
            } else if (mac_mode_ == MacMode::Observed) {
                use.full = false;
                use.mac_reader = use.mac_exit = false;
                if (analysis.sets_flags) {
                    const Observed seen = observe(pc, true);
                    use.mac_reader = seen.reader;
                    use.mac_exit = seen.exit;
                }
            }
            if (!analysis.clip) {
                use.clip_reader = false;
            } else if (clip_all) {
                use.clip_reader = true;
            } else {
                use.clip_reader = observe(pc, false).reader;
            }
            use_[pc] = use;
            if (analysis.sets_flags) {
                ++record_.mac_writers;
                if (use.full || use.mac_reader || use.mac_exit) ++record_.mac_sites;
                if (use.full || use.mac_reader) ++record_.mac_reader_sites;
            }
            if (analysis.clip) {
                ++record_.clip_writers;
                ++record_.clip_sites;
                if (use.clip_reader) ++record_.clip_reader_sites;
            }
        }
    }

    // Walks the program from one write and reports whether a reader of its channel
    // can see it, and whether it is the last write of its channel before an exit on
    // some path. State is the distance from the first later write of the channel:
    // none before any, then 0 .. window - 1. A reader at a state >= window does not
    // see the write, and a path that reaches that state is settled. Backward
    // transfers are also exits, because the budget check may leave there.
    Observed observe(std::uint32_t write_pc, bool mac) {
        constexpr int kNone = -1;
        const int window = mac ? kMacWindow : kClipWindow;
        ++stamp_;
        Observed seen;
        std::vector<std::pair<std::uint32_t, int>> work;
        const auto edge = [&](std::int64_t target, int state) {
            if (target < 0 || !in_.count(static_cast<std::uint32_t>(target))) {
                if (state == kNone) seen.exit = true;
                return;
            }
            work.emplace_back(static_cast<std::uint32_t>(target), state);
        };
        // A backward transfer is also an exit, because the budget check may leave there.
        const auto leave = [&](std::uint32_t from, std::int64_t next, int after) {
            if (next >= 0 && static_cast<std::uint32_t>(next) <= from && after == kNone) seen.exit = true;
            edge(next, after);
        };
        for (std::int64_t next : successors(write_pc)) leave(write_pc, next, kNone);
        while (!work.empty()) {
            const std::pair<std::uint32_t, int> item = work.back();
            work.pop_back();
            const std::uint32_t node = item.first;
            const int state = item.second;
            std::uint32_t &mark = stamps_[static_cast<std::size_t>(node >> 3) * kStateSlots + static_cast<std::size_t>(state + 1)];
            if (mark == stamp_) continue;
            mark = stamp_;
            if (lower_exit(node)) {
                if (state == kNone) seen.exit = true;
                continue;
            }
            // A reader sees the write unless a later write is at least window instructions before it.
            if (channel_reader(node, mac) && (state == kNone || state < window)) seen.reader = true;
            const int here = state == kNone && channel_writer(node, mac) ? 0 : state;
            const int after = here == kNone ? kNone : here + 1;
            if (after >= window) continue;
            for (std::int64_t next : successors(node)) leave(node, next, after);
        }
        return seen;
    }

    // Upper bound on instructions executed on a forward-only path from target.
    // Forward edges strictly increase the masked PC, so a path visits each
    // node at most once and the reachable count bounds its length.
    std::size_t forward_region_size(std::uint32_t target) const {
        std::set<std::uint32_t> seen{target};
        std::vector<std::uint32_t> work{target};
        while (!work.empty()) {
            const std::uint32_t pc = work.back();
            work.pop_back();
            for (std::int64_t s : successors(pc)) {
                if (s < 0) continue;
                const auto next = static_cast<std::uint32_t>(s);
                if (next > pc && in_.count(next) && seen.insert(next).second) work.push_back(next);
            }
        }
        return seen.size();
    }

    // Charges the budget for arrival at a backward target. Returns C text.
    std::string charge(std::uint32_t target) {
        const std::size_t weight = forward_region_size(target);
        ++record_.budget_heads;
        uses_exhausted_ = true;
        return std::format("if (ctx.budget < {}u) {{ ctx.resume_pc = {}u; goto vugen_exhausted; }} ctx.budget -= {}u; ", weight, target, weight);
    }

    std::string generate() {
        std::string o = std::format("RunStatus {}(Vu0Regs &r, std::uint8_t *mem, RunContext &ctx) {{\n", record_.function_name);
        o += "    FlagPipe fl;\n    fl.start(r);\n";
        o += "    bool pend = false, pendj = false, ending = false;\n";
        o += "    std::uint32_t jpc = 0;\n    (void)pend;\n    (void)pendj;\n    (void)ending;\n    (void)jpc;\n    (void)mem;\n";
        // Exit state: MAC is kept in last_mac in Observed mode, and clip in last_clip. Both start
        // from the incoming state and are written back at every exit (VUGEN_SYNC).
        if (mac_mode_ == MacMode::Observed) o += "    std::uint32_t last_mac = r.mac;\n    (void)last_mac;\n";
        o += "    std::uint32_t last_clip = r.clip;\n    (void)last_clip;\n";
        for (int reg : shadowed_) o += std::format("    v4u {} = vfix(vload({}));\n", reg == kAcc ? "nacc" : std::format("n{}", reg), reg == kAcc ? "r.acc" : std::format("r.vf[{}]", reg));
        o += "    VUGEN_LOCALS;\n";
        o += "    " + charge(record_.entry);
        o += std::format("goto {};\n", label(record_.entry));
        for (std::uint32_t pc : pcs_) o += block(pc);
        if (uses_dispatch_) o += dispatch_text();
        if (uses_exhausted_) o += "vugen_exhausted:\n    VUGEN_SYNC; return RunStatus::BudgetExhausted;\n";
        o += "}\n\n";
        // VU0 data memory is 4 KiB: the emitted qword accesses use the 256-qword mask.
        if (profile_.unit == 0) replace_all(o, "qword(mem, ", "qword_vu0(mem, ");
        std::string exit_sync = "r.clip = last_clip; ";
        if (mac_mode_ == MacMode::Observed) exit_sync = "r.mac = last_mac; " + exit_sync;
        return localize_vi(o, exit_sync);
    }

    std::string block(std::uint32_t pc) {
        const std::uint32_t lower_word = lo(pc), upper_word = hi(pc);
        const bool immediate = upper_word & kImmediateBit;
        const Lower &l = lowers_[pc];
        std::string o;
        if (lower_exit(pc)) {
            o += std::format("{}: {{ VUGEN_SYNC; {}; return RunStatus::Completed; }}\n", label(pc), exit_call(std::format("{}u", pc)));
        } else {
            const Upper refreshed = upper(upper_word, use_.at(pc), profile_.unit == 0);
            o +=std::format("{}: {{ ++fl.t; ", label(pc));
            o += refreshed.pre;
            if (immediate) o += std::format("r.i = {}u; ", lower_word);
            else o += l.code;
            o += refreshed.post;
            const int lower_write = immediate ? -1 : l.writes;
            if (lower_write >= 0 && shadowed_.count(lower_write)) o += std::format("{} = vfix(vload({})); ", shadow_name(lower_write), raw_name(lower_write));
            if (refreshed.target >= 0 && refreshed.target != lower_write && shadowed_.count(refreshed.target)) {
                if (!refreshed.normalized) o += std::format("{} = vfix(vload({})); ", shadow_name(refreshed.target), raw_name(refreshed.target));
                else if (refreshed.mask == 0xF) o += std::format("{} = uv; ", shadow_name(refreshed.target));
                else o += std::format("{0} = vsel({0}, uv, {1}); ", shadow_name(refreshed.target), lane_mask(refreshed.mask));
            }
            if (upper_word & kEndBit) o += "ending = true; ";
            o += "}\n";
        }
        const std::uint32_t prev = (pc - 8u) & mask_;
        if (in_.count(prev)) {
            const Lower &previous = lowers_[prev];
            const std::uint32_t previous_hi = hi(prev);
            // TASK-134: an E-bit instruction that is also a branch (Spec II VU0, 0x3B0) stops at the branch target, as the interpreter.
            if (previous_hi & kEndBit) {
                if (previous.branch) o += std::format("    if (ending) {{ VUGEN_SYNC; r.tpc = pend ? {}u : {}u; return RunStatus::Completed; }}\n", previous.target, (pc + 8u) & mask_);
                else o += std::format("    if (ending) {{ VUGEN_SYNC; r.tpc = {}u; return RunStatus::Completed; }}\n", (pc + 8u) & mask_);
            }
            if (previous.branch) {
                const std::uint32_t target = previous.target;
                o += "    if (pend) { pend = false; ";
                if (target <= pc) o += charge(target);
                o += std::format("goto {}; }}\n", label(target));
            }
            if (previous.jump) {
                uses_dispatch_ = true;
                o += "    if (pendj) { pendj = false; goto dispatch; }\n";
            }
        }
        const std::uint32_t next = (pc + 8u) & mask_;
        if (!in_.count(next)) {
            o += std::format("    VUGEN_SYNC; {}; return RunStatus::Completed;\n", exit_call(std::format("{}u", next)));
        } else if (next <= pc) {
            o += "    " + charge(next);
            o += std::format("goto {};\n", label(next));
        }
        return o;
    }

    static std::string shadow_name(int reg) { return reg == kAcc ? "nacc" : std::format("n{}", reg); }
    static std::string raw_name(int reg) { return reg == kAcc ? "r.acc" : std::format("r.vf[{}]", reg); }
};

bool is_lower_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }

bool parse_hex(const std::string &text, std::uint64_t &value) {
    value = 0;
    for (char c : text) {
        if (!is_lower_hex(c)) return false;
        value = value * 16u + static_cast<std::uint64_t>(c <= '9' ? c - '0' : c - 'A' + 10);
    }
    return true;
}

std::string json_escape(const std::string &text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

}  // namespace

bool parse_input_file_name(const std::string &name, std::uint64_t &key, std::uint32_t &entry, std::string &error) {
    // KEY16HEX_ENTRY4HEX.bin, for example 0123456789ABCDEF_0040.bin.
    const std::string expected_suffix = ".bin";
    if (name.size() != 16 + 1 + 4 + expected_suffix.size() || name.compare(name.size() - 4, 4, expected_suffix) != 0 || name[16] != '_') {
        error = std::format("input name must be KEY16HEX_ENTRY4HEX.bin: {}", name);
        return false;
    }
    const std::string key_text = name.substr(0, 16), entry_text = name.substr(17, 4);
    std::uint64_t entry_value = 0;
    if (!parse_hex(key_text, key) || !parse_hex(entry_text, entry_value)) {
        error = std::format("input name has non-hex or lowercase digits: {}", name);
        return false;
    }
    if (entry_value > kCodeMask || (entry_value & 7u) != 0) {
        error = std::format("entry must be an 8-byte aligned address below 0x4000: {}", name);
        return false;
    }
    entry = static_cast<std::uint32_t>(entry_value);
    return true;
}

bool read_input_image(const fs::path &path, std::vector<std::uint8_t> &image, std::string &error, std::size_t image_bytes) {
    std::error_code size_error;
    const std::uintmax_t size = fs::file_size(path, size_error);
    if (size_error) {
        error = std::format("cannot stat {}: {}", path.filename().string(), size_error.message());
        return false;
    }
    if (size != image_bytes) {
        error = std::format("{}: size {} is not {} bytes", path.filename().string(), static_cast<unsigned long long>(size), image_bytes);
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = std::format("cannot open {}", path.filename().string());
        return false;
    }
    image.assign(image_bytes, 0);
    file.read(reinterpret_cast<char *>(image.data()), static_cast<std::streamsize>(image.size()));
    if (file.gcount() != static_cast<std::streamsize>(image_bytes) || file.bad()) {
        error = std::format("{}: short read", path.filename().string());
        return false;
    }
    return true;
}

namespace {
bool g_flag_elision = true;
}

void set_flag_elision(bool enabled) { g_flag_elision = enabled; }
bool flag_elision() { return g_flag_elision; }

std::string program_digest(const std::vector<std::uint8_t> &image, const std::vector<std::uint32_t> &reachable, std::uint32_t entry, bool flags_used, bool status_used,
                          const std::string &tag) {
    return program_identity_digest(image.data(), reachable, entry, flags_used, status_used, tag);
}

bool add_program_record(std::vector<ProgramRecord> &records, ProgramRecord candidate, std::string &error) {
    for (const ProgramRecord &existing : records) {
        if (existing.key != candidate.key) continue;
        if (existing.digest_sha256 == candidate.digest_sha256) return true;
        error = std::format("key collision: key 0x{:016X} has two different reachable identities", candidate.key);
        return false;
    }
    records.push_back(std::move(candidate));
    return true;
}

bool collect_program_records(const fs::path &input_dir, std::vector<ProgramRecord> &records, std::string &error) {
    std::error_code directory_error;
    if (!fs::is_directory(input_dir, directory_error)) {
        error = "input is not a directory";
        return false;
    }
    std::vector<fs::path> files;
    for (const auto &item : fs::directory_iterator(input_dir, directory_error)) files.push_back(item.path());
    if (directory_error) {
        error = "cannot list input directory";
        return false;
    }
    std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) { return a.filename().string() < b.filename().string(); });

    std::vector<ProgramRecord> recorded;
    for (const fs::path &file : files) {
        const std::string name = file.filename().string();
        std::uint64_t key = 0;
        std::uint32_t entry = 0;
        if (!parse_input_file_name(name, key, entry, error)) return false;
        std::error_code type_error;
        if (!fs::is_regular_file(file, type_error)) {
            error = std::format("{} is not a regular file", name);
            return false;
        }
        ProgramRecord record;
        if (!read_input_image(file, record.image, error)) return false;
        record.reachable = ps2::vu::reachable(record.image.data(), kMask, entry);
        const std::uint64_t computed = ps2::vu::program_key(record.image.data(), record.reachable, entry);
        if (computed != key) {
            error = std::format("{}: key mismatch, file name says 0x{:016X} and content gives 0x{:016X}", name, key, computed);
            return false;
        }
        record.key = key;
        record.entry = entry;
        record.origin = "recorded";
        record.source_name = name;
        record.source_sha256 = sha256_hex(record.image);
        record.image_flags_used = image_flags_used(record.image.data(), kImageBytes);
        record.image_status_used = image_status_used(record.image.data(), kImageBytes);
        record.digest_sha256 = program_digest(record.image, record.reachable, entry, record.image_flags_used, record.image_status_used);
        if (!add_program_record(records, record, error)) return false;
        recorded.push_back(std::move(record));
    }

    // Jump targets: continuation of JAL/JALR links and constant loads into vi0-based
    // registers, the same discovery rule as the donor emitter.
    for (const ProgramRecord &parent : recorded) {
        std::set<std::uint32_t> targets;
        for (std::uint32_t pc : parent.reachable) {
            const std::uint32_t lower_word = word(parent.image, pc), upper_word = word(parent.image, pc + 4);
            if (upper_word & kImmediateBit) continue;
            const std::uint32_t op = lower_word >> 25;
            if (op == 0x21 || op == 0x25) targets.insert((pc + 16u) & kMask);
            if (op == 0x08 && ((lower_word >> 11) & 15u) == 0) targets.insert(((((lower_word >> 10) & 0x7800u) | (lower_word & 0x7FFu)) * 8u) & kMask);
        }
        for (std::uint32_t target : targets) {
            ProgramRecord derived;
            derived.image = parent.image;
            derived.entry = target;
            derived.reachable = ps2::vu::reachable(derived.image.data(), kMask, target);
            derived.key = ps2::vu::program_key(derived.image.data(), derived.reachable, target);
            derived.origin = "jump_target";
            derived.source_name = parent.source_name;
            derived.source_sha256 = parent.source_sha256;
            derived.image_flags_used = parent.image_flags_used;
            derived.image_status_used = parent.image_status_used;
            derived.digest_sha256 = program_digest(derived.image, derived.reachable, target, derived.image_flags_used, derived.image_status_used);
            if (!add_program_record(records, std::move(derived), error)) return false;
        }
    }
    std::sort(records.begin(), records.end(), [](const ProgramRecord &a, const ProgramRecord &b) { return a.key < b.key; });
    return true;
}

bool read_entry_list(const fs::path &path, std::vector<std::uint32_t> &entries, std::string &error, std::uint32_t code_mask) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open entry list";
        return false;
    }
    std::set<std::uint32_t> unique;
    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::uint64_t value = 0;
        if (line.size() != 4 || !parse_hex(line, value)) {
            error = std::format("entry list line {} is not a 4-digit uppercase hex address: {}", line_number, line);
            return false;
        }
        if (value > code_mask || (value & 7u) != 0) {
            error = std::format("entry list line {} must be an 8-byte aligned address below 0x4000: {}", line_number, line);
            return false;
        }
        unique.insert(static_cast<std::uint32_t>(value));
    }
    if (file.bad()) {
        error = "cannot read entry list";
        return false;
    }
    if (unique.empty()) {
        error = "entry list has no entries";
        return false;
    }
    entries.assign(unique.begin(), unique.end());
    return true;
}

bool collect_explicit_program_records(const std::vector<std::uint8_t> &image, const std::vector<std::uint32_t> &entries,
                                      std::vector<ProgramRecord> &records, std::string &error, const UnitProfile &profile) {
    if (image.size() != profile.image_bytes) {
        error = std::format("image must be {} bytes", profile.image_bytes);
        return false;
    }
    // Image-wide flag and status use is computed once: it is part of every entry's identity. VU0 forces both on.
    const bool flags_used = profile.exact_flags || image_flags_used(image.data(), profile.image_bytes);
    const bool status_used = profile.exact_flags || image_status_used(image.data(), profile.image_bytes);
    const std::string image_sha256 = sha256_hex(image);
    for (std::uint32_t entry : entries) {
        if (entry >= profile.image_bytes) {
            error = std::format("entry 0x{:04X} is outside the {}-byte image", entry, profile.image_bytes);
            return false;
        }
        ProgramRecord record;
        record.image = image;
        record.entry = entry;
        record.unit = profile.unit;
        record.reachable = ps2::vu::reachable(record.image.data(), profile.code_mask(), entry);
        record.key = ps2::vu::program_key(record.image.data(), record.reachable, entry);
        record.origin = "explicit";
        record.source_name = "image";
        record.source_sha256 = image_sha256;
        record.image_flags_used = flags_used;
        record.image_status_used = status_used;
        record.digest_sha256 = program_digest(record.image, record.reachable, entry, flags_used, status_used, profile.identity_tag);
        if (!add_program_record(records, std::move(record), error)) return false;
    }
    std::sort(records.begin(), records.end(), [](const ProgramRecord &a, const ProgramRecord &b) { return a.key < b.key; });
    return true;
}

bool prepare_fresh_output_directory(const fs::path &directory, std::string &error) {
    std::error_code status_error;
    if (fs::exists(directory, status_error)) {
        if (!fs::is_directory(directory, status_error)) {
            error = "output path exists and is not a directory";
            return false;
        }
        if (!fs::is_empty(directory, status_error)) {
            error = "output directory exists and is not empty; use a fresh directory";
            return false;
        }
        return true;
    }
    std::error_code create_error;
    fs::create_directories(directory, create_error);
    if (create_error) {
        error = "cannot create output directory";
        return false;
    }
    return true;
}

std::string emit_program_function(ProgramRecord &record) {
    FunctionGenerator generator(record);
    return generator.run();
}

std::string emit_generated_source(std::vector<ProgramRecord> &records, const std::string &name_space) {
    std::string functions;
    std::string table;
    std::size_t emitted = 0;
    for (ProgramRecord &record : records) {
        const std::string text = emit_program_function(record);
        if (record.refused) continue;
        functions += text;
        table += std::format("    {{0x{:016X}ull, {}u, {}, {}, \"{}\", {}}},\n", record.key, record.entry, record.image_flags_used ? "true" : "false",
                             record.image_status_used ? "true" : "false", record.digest_sha256, record.function_name);
        ++emitted;
    }
    std::string source = "// Generated by gt4_vu_recomp. Do not edit.\n";
    source += "#include \"vu_recomp_contract.hpp\"\n";
    if (functions.find("fp4::") != std::string::npos) source += "#include \"ps2/fp4.hpp\"\n";  // TASK-134, VU0 lanes
    source += "\n";
    source += std::format("namespace {} {{\n\nusing namespace ps2;\nusing namespace ps2::vuops;\n\n", name_space);
    for (const ProgramRecord &record : records) {
        if (record.refused) continue;
        source += std::format("RunStatus {}(Vu0Regs &r, std::uint8_t *mem, RunContext &ctx);\n", record.function_name);
    }
    source += "\n" + functions;
    source += "extern const ProgramEntry kPrograms[] = {\n" + table;
    if (emitted == 0) source += "    {0u, 0u, false, false, \"\", nullptr},\n";
    source += "};\n";
    source += std::format("extern const std::uint32_t kProgramCount = {}u;\n\n}}\n", emitted);
    return source;
}

std::string emit_generated_split(std::vector<ProgramRecord> &records, std::size_t programs_per_file, std::vector<GeneratedPart> &parts,
                                 const std::string &name_space) {
    const std::string header = std::format("// Generated by gt4_vu_recomp. Do not edit.\n#include \"vu_recomp_contract.hpp\"\n\n"
                                           "namespace {} {{\n\nusing namespace ps2;\nusing namespace ps2::vuops;\n\n",
                                           name_space);
    std::vector<std::string> bodies;
    std::vector<std::string> names;
    std::string table;
    for (ProgramRecord &record : records) {
        const std::string text = emit_program_function(record);
        if (record.refused) continue;
        bodies.push_back(text);
        names.push_back(record.function_name);
        table += std::format("    {{0x{:016X}ull, {}u, {}, {}, \"{}\", {}}},\n", record.key, record.entry, record.image_flags_used ? "true" : "false",
                             record.image_status_used ? "true" : "false", record.digest_sha256, record.function_name);
    }
    const std::size_t chunk = programs_per_file == 0 ? (bodies.empty() ? 1 : bodies.size()) : programs_per_file;
    parts.clear();
    const std::string file_prefix = unit_profile(records.empty() ? 1 : records.front().unit).file_prefix;
    for (std::size_t first = 0; first < bodies.size(); first += chunk) {
        const std::size_t last = std::min(bodies.size(), first + chunk);
        GeneratedPart part;
        part.file_name = std::format("{}_{:03}.cpp", file_prefix, parts.size());
        part.programs = last - first;
        part.text = header;
        for (std::size_t i = first; i < last; ++i) part.text += std::format("RunStatus {}(Vu0Regs &r, std::uint8_t *mem, RunContext &ctx);\n", names[i]);
        part.text += "\n";
        for (std::size_t i = first; i < last; ++i) part.text += bodies[i];
        part.text += "\n}\n";
        parts.push_back(std::move(part));
    }
    std::string source = header;
    for (const std::string &name : names) source += std::format("RunStatus {}(Vu0Regs &r, std::uint8_t *mem, RunContext &ctx);\n", name);
    source += "\nextern const ProgramEntry kPrograms[] = {\n" + table;
    if (names.empty()) source += "    {0u, 0u, false, false, \"\", nullptr},\n";
    source += "};\n";
    source += std::format("extern const std::uint32_t kProgramCount = {}u;\n\n}}\n", names.size());
    return source;
}

std::string manifest_json(const std::vector<ProgramRecord> &records, const std::string &generated_file_name, const std::string &generated_sha256,
                          const ManifestInput &input) {
    std::size_t functions = 0;
    for (const ProgramRecord &record : records)
        if (!record.refused) ++functions;
    std::string json = "{\n  \"tool\": \"gt4_vu_recomp\",\n  \"donor_commit\": \"1b4808793a12c45125262fd2d5f50a9ec9f7c020\",\n";
    json += std::format("  \"input\": \"{}\",\n", json_escape(input.kind));
    if (input.kind == "explicit_entries")
        json += std::format("  \"image_sha256\": \"{}\",\n  \"entry_list_sha256\": \"{}\",\n", input.image_sha256, input.entry_list_sha256);
    json += std::format("  \"generated_file\": \"{}\",\n  \"generated_sha256\": \"{}\",\n", json_escape(generated_file_name), generated_sha256);
    json += std::format("  \"generated_bytes\": {},\n  \"entry_count\": {},\n  \"function_count\": {},\n", input.generated_bytes, records.size(), functions);
    json += std::format("  \"flag_elision\": {},\n", flag_elision() ? "true" : "false");
    json += std::format("  \"entry_policy\": \"{}\",\n  \"programs_per_file\": {},\n", json_escape(input.entry_policy), input.programs_per_file);
    if (!input.parts.empty()) {
        json += "  \"generated_parts\": [\n";
        for (std::size_t i = 0; i < input.parts.size(); ++i) {
            const GeneratedPart &part = input.parts[i];
            json += std::format("    {{\"file\": \"{}\", \"sha256\": \"{}\", \"bytes\": {}, \"programs\": {}}}{}\n", json_escape(part.file_name),
                                sha256_hex(reinterpret_cast<const std::uint8_t *>(part.text.data()), part.text.size()), part.text.size(), part.programs,
                                i + 1 < input.parts.size() ? "," : "");
        }
        json += "  ],\n";
    }
    if (input.unit != 1) {
        json += std::format("  \"unit\": {},\n  \"identity_tag\": \"{}\",\n", input.unit, unit_profile(input.unit).identity_tag);
    }
    json += std::format("  \"instruction_budget\": {},\n  \"programs\": [\n", 1000000u);
    for (std::size_t i = 0; i < records.size(); ++i) {
        const ProgramRecord &record = records[i];
        json += "    {";
        json += std::format("\"key\": \"0x{:016X}\", \"entry\": {}, \"origin\": \"{}\", ", record.key, record.entry, record.origin);
        json += std::format("\"digest_sha256\": \"{}\", \"reachable_instructions\": {}, \"image_flags_used\": {}, \"image_status_used\": {}, ", record.digest_sha256,
                            record.reachable.size(), record.image_flags_used ? "true" : "false", record.image_status_used ? "true" : "false");
        json += std::format("\"interpreter_calls\": {}, ", record.interpreter_calls);
        json += std::format("\"mac_mode\": \"{}\", \"mac_writers\": {}, \"mac_sites\": {}, \"mac_reader_sites\": {}, ", record.mac_mode, record.mac_writers,
                            record.mac_sites, record.mac_reader_sites);
        json += std::format("\"clip_writers\": {}, \"clip_sites\": {}, \"clip_reader_sites\": {}, ", record.clip_writers, record.clip_sites, record.clip_reader_sites);
        json += std::format("\"fallback_sites\": {}, \"upper_nop_sites\": {}, \"budget_heads\": {}, ", record.fallback_sites, record.upper_nop_sites, record.budget_heads);
        json += "\"exit_pcs\": [";
        for (std::size_t k = 0; k < record.exit_pcs.size(); ++k) json += std::format("{}{}", k ? ", " : "", record.exit_pcs[k]);
        json += "], ";
        json += std::format("\"status\": \"{}\", ", record.refused ? "refused" : "emitted");
        json += std::format("\"refusal\": \"{}\", \"function\": \"{}\"", json_escape(record.refusal), json_escape(record.function_name));
        json += i + 1 < records.size() ? "},\n" : "}\n";
    }
    json += "  ]\n}\n";
    return json;
}

}
