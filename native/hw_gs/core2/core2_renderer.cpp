// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070). Section numbers in comments refer to CORE2-SPEC.

#include "core2_renderer.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <bit>
#include <chrono>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core2_gpu.hpp"
#include "core2_replace.hpp"
#include "core2_upload.hpp"
#include "hw_batch.hpp"
#include "hw_clut.hpp"
#include "hw_front_end.hpp"
#include "hw_gs_display.hpp"
#include "hw_gs_registers.hpp"
#include "hw_gs_types.hpp"
#include "hw_gs_lod.hpp"
#include "hw_memory.hpp"
#include "hw_gs_replace/clut.hpp"
#include "hw_gs_replace/gs_types.hpp"
#include "hw_gs_replace/keys.hpp"
#include "hw_gs_replace/names.hpp"
#include "hw_gs_replace/packs.hpp"
#include "../cpuref/cpuref_draw.hpp"
#include "../cpuref/cpuref_memory.hpp"

namespace hwgs::core2 {
namespace {

using memory::BufferView;
using memory::Psm;

constexpr std::uint32_t kAllBlocks = memory::kBlocksTotal;
constexpr std::uint32_t kAgeLimitFrames = 60;      // 6.2
constexpr std::uint64_t kRecentUploadDraws = 500;  // 2.4b

namespace priv {
constexpr std::uint32_t kPage = 0x12000000u;
constexpr std::uint32_t kPmode = kPage + 0x00;
constexpr std::uint32_t kSmode1 = kPage + 0x10;
constexpr std::uint32_t kSmode2 = kPage + 0x20;
constexpr std::uint32_t kSyncv = kPage + 0x60;
constexpr std::uint32_t kDispfb1 = kPage + 0x70;
constexpr std::uint32_t kDisplay1 = kPage + 0x80;
constexpr std::uint32_t kDispfb2 = kPage + 0x90;
constexpr std::uint32_t kDisplay2 = kPage + 0xA0;
constexpr std::uint32_t kBgcolor = kPage + 0xE0;
constexpr std::uint32_t kCsr = kPage + 0x1000;
}  // namespace priv

// 1.3: format classes. 32- and 24-bit colour share a target, as do Z32 and Z24.
enum class FormatClass : std::uint8_t { Colour32, Colour16, Depth32, Depth16, None };

FormatClass ClassOf(Psm psm) {
    switch (psm) {
        case Psm::Ct32:
        case Psm::Ct24: return FormatClass::Colour32;
        case Psm::Ct16:
        case Psm::Ct16S: return FormatClass::Colour16;
        case Psm::Z32:
        case Psm::Z24: return FormatClass::Depth32;
        case Psm::Z16:
        case Psm::Z16S: return FormatClass::Depth16;
        default: return FormatClass::None;
    }
}

bool IsDepthClass(FormatClass c) { return c == FormatClass::Depth32 || c == FormatClass::Depth16; }

// Bits of a stored word a write in `psm` owns: a 24-bit view leaves the top byte to the 32-bit one.
std::uint32_t WriteMaskOf(Psm psm) {
    return psm == Psm::Ct24 || psm == Psm::Z24 ? 0x00FFFFFFu : 0xFFFFFFFFu;
}

PixelRect Union(const PixelRect &a, const PixelRect &b) {
    if (a.Empty()) return b;
    if (b.Empty()) return a;
    return PixelRect{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

PixelRect Intersect(const PixelRect &a, const PixelRect &b) {
    return PixelRect{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

bool Contains(const PixelRect &outer, const PixelRect &inner) {
    return !outer.Empty() && outer.x0 <= inner.x0 && outer.y0 <= inner.y0 && outer.x1 >= inner.x1 && outer.y1 >= inner.y1;
}

// A conservative range of GS blocks (modulo 16384) a rectangle of a buffer can touch: whole page rows, plus one page
// when the base is not page aligned; count >= 16384 means "all memory" (the same rule as the old core's spans).
struct BlockSpan {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
};

BlockSpan SpanOfRect(const BufferView &view, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) {
    if (x1 < x0 || y1 < y0) return BlockSpan{};
    if (x0 < 0 || y0 < 0 || x1 >= 2048 || y1 >= 2048) return BlockSpan{0, kAllBlocks};
    const memory::PsmInfo &info = memory::psmInfo(view.psm);
    const std::uint32_t bufferWidth = std::max<std::uint32_t>(view.bufferWidth, 1u) * 64u;
    const std::uint32_t pagesPerRow = std::max<std::uint32_t>(1u, (bufferWidth + info.pageWidth - 1u) / info.pageWidth);
    const std::uint32_t firstRow = static_cast<std::uint32_t>(y0) / info.pageHeight;
    std::uint32_t rows = static_cast<std::uint32_t>(y1) / info.pageHeight - firstRow + 1u;
    if (static_cast<std::uint32_t>(x1) >= bufferWidth) rows += 1u + static_cast<std::uint32_t>(x1) / bufferWidth;
    const std::uint64_t spill = (view.basePointer % memory::kBlocksPerPage) != 0 ? memory::kBlocksPerPage : 0;
    const std::uint64_t count = static_cast<std::uint64_t>(rows) * pagesPerRow * memory::kBlocksPerPage + spill;
    if (count >= kAllBlocks) return BlockSpan{0, kAllBlocks};
    return BlockSpan{(view.basePointer + firstRow * pagesPerRow * memory::kBlocksPerPage) % kAllBlocks,
                     static_cast<std::uint32_t>(count)};
}

BlockSpan SpanOfRect(const BufferView &view, const PixelRect &r) { return SpanOfRect(view, r.x0, r.y0, r.x1, r.y1); }

// The span of a write into GS memory (EE uploads, moves): exact blocks for a small rectangle, else SpanOfRect.
BlockSpan WriteSpan(const BufferView &view, const PixelRect &r) {
    const std::int32_t x0 = r.x0, y0 = r.y0, x1 = r.x1, y1 = r.y1;
    if (x1 < x0 || y1 < y0) return BlockSpan{};
    if (x0 < 0 || y0 < 0 || x1 >= 2048 || y1 >= 2048) return BlockSpan{0, kAllBlocks};
    const memory::PsmInfo &info = memory::psmInfo(view.psm);
    // A small rectangle gets its exact blocks: the conservative page rows below add a whole page when the base is
    // not page aligned, and near the end of memory that page wraps to block 0 (GT4 writes 8 x 8 pixels at block
    // 16328: the spill page reached the display buffer at block 0 and its top rows were patched from memory).
    {
        const std::int32_t bw = info.blockWidth, bh = info.blockHeight;
        const std::int64_t cells = static_cast<std::int64_t>(x1 / bw - x0 / bw + 1) * (y1 / bh - y0 / bh + 1);
        if (cells <= 1024) {
            std::uint32_t first = kAllBlocks, last = 0;
            for (std::int32_t by = y0 / bh; by <= y1 / bh; ++by) {
                for (std::int32_t bx = x0 / bw; bx <= x1 / bw; ++bx) {
                    const std::uint32_t block = BlockOfPixel(view, static_cast<std::uint32_t>(bx * bw),
                                                             static_cast<std::uint32_t>(by * bh));  // IN-456
                    first = std::min(first, block);
                    last = std::max(last, block);
                }
            }
            // A range that wraps the end of memory (first near 0, last near the top) stays conservative.
            if (last - first < kAllBlocks / 2) return BlockSpan{first, last - first + 1u};
        }
    }
    return SpanOfRect(view, r);
}

bool SpansOverlap(const BlockSpan &a, const BlockSpan &b) {
    if (a.count == 0 || b.count == 0) return false;
    if (a.count >= kAllBlocks || b.count >= kAllBlocks) return true;
    const std::uint32_t ab = (b.begin + kAllBlocks - a.begin) % kAllBlocks;
    const std::uint32_t ba = (a.begin + kAllBlocks - b.begin) % kAllBlocks;
    return ab < a.count || ba < b.count;
}

// Whether every block of `inner` lies in `outer` (modulo the 16384 blocks of GS memory).
bool SpanContains(const BlockSpan &outer, const BlockSpan &inner) {
    if (inner.count == 0 || outer.count >= kAllBlocks) return true;
    if (outer.count == 0) return false;
    const std::uint32_t offset = (inner.begin + kAllBlocks - outer.begin) % kAllBlocks;
    return static_cast<std::uint64_t>(offset) + inner.count <= outer.count;
}

// IN-415: the blocks of the whole pages a write of `rect` in `view` replaces in every bit. That needs a format that
// stores each pixel in a whole storage unit (not CT24, Z24, T8H, T4HL or T4HH), a rectangle over the whole buffer
// width, and only complete page rows count. Empty when the write replaces no whole page.
BlockSpan WholePagesWritten(const BufferView &view, const PixelRect &rect) {
    const memory::PsmInfo &info = memory::psmInfo(view.psm);
    if (info.storageBits != info.transferBpp) return BlockSpan{};
    // The Z block swap stays inside a page only when the base pointer is page aligned.
    if (info.isDepth && view.basePointer % memory::kBlocksPerPage != 0) return BlockSpan{};
    const std::uint32_t width = view.bufferWidth * 64u;
    if (width == 0 || width > 2048u || width % info.pageWidth != 0) return BlockSpan{};
    if (rect.x0 != 0 || rect.x1 + 1 != static_cast<std::int32_t>(width) || rect.y0 < 0 || rect.y1 >= 2048) {
        return BlockSpan{};
    }
    const std::uint32_t firstRow = (static_cast<std::uint32_t>(rect.y0) + info.pageHeight - 1u) / info.pageHeight;
    const std::uint32_t endRow = (static_cast<std::uint32_t>(rect.y1) + 1u) / info.pageHeight;
    if (endRow <= firstRow) return BlockSpan{};
    const std::uint32_t perRow = width / info.pageWidth;
    const std::uint64_t count = static_cast<std::uint64_t>(endRow - firstRow) * perRow * memory::kBlocksPerPage;
    return BlockSpan{(view.basePointer + firstRow * perRow * memory::kBlocksPerPage) % kAllBlocks,
                     static_cast<std::uint32_t>(std::min<std::uint64_t>(count, kAllBlocks))};
}

// IN-416: the block cells of one page of a PSM: the block number of each cell and the cell of each block number.
// `hierarchical` says every pixel's byte and bit inside its block depend only on its place in the block cell and its
// block only on the cell, which makes a write's pixels land at the same in-block places in any view of that PSM
// family, whatever the base pointer (checked here from memory::pageLocalSlot, not assumed).
struct BlockGrid {
    std::uint32_t cellsX = 0, cellsY = 0;
    std::array<std::uint8_t, 32> blockOfCell{};
    std::array<std::uint8_t, 32> cellOfBlock{};
    bool hierarchical = false;
};

const BlockGrid &BlockGridOf(Psm psm) {
    static const std::array<BlockGrid, 64> grids = [] {
        std::array<BlockGrid, 64> built{};
        for (std::uint32_t code = 0; code < 64u; ++code) {
            const auto psm = memory::decodePsm(code);
            if (!psm) continue;
            const memory::PsmInfo &info = memory::psmInfo(*psm);
            if (info.zBlockXorApplies) continue;  // the Z block swap moves blocks across base pointers
            BlockGrid &grid = built[code];
            grid.cellsX = info.pageWidth / info.blockWidth;
            grid.cellsY = info.pageHeight / info.blockHeight;
            if (grid.cellsX * grid.cellsY != memory::kBlocksPerPage) continue;
            bool ok = true;
            for (std::uint32_t y = 0; y < info.pageHeight && ok; ++y) {
                for (std::uint32_t x = 0; x < info.pageWidth && ok; ++x) {
                    const memory::PixelSlot slot = memory::pageLocalSlot(*psm, x, y);
                    const memory::PixelSlot corner = memory::pageLocalSlot(*psm, x % info.blockWidth, y % info.blockHeight);
                    const memory::PixelSlot cellCorner =
                        memory::pageLocalSlot(*psm, x - x % info.blockWidth, y - y % info.blockHeight);
                    ok = slot.byteOffset / memory::kBlockBytes == cellCorner.byteOffset / memory::kBlockBytes &&
                         slot.byteOffset % memory::kBlockBytes == corner.byteOffset % memory::kBlockBytes &&
                         slot.bitShift == corner.bitShift;
                }
            }
            std::array<bool, 32> seen{};
            for (std::uint32_t cell = 0; cell < memory::kBlocksPerPage && ok; ++cell) {
                const std::uint32_t block = memory::pageLocalSlot(*psm, (cell % grid.cellsX) * info.blockWidth,
                                                                  (cell / grid.cellsX) * info.blockHeight)
                                                .byteOffset /
                                            memory::kBlockBytes;
                ok = block < 32u && !seen[block];
                if (!ok) break;
                seen[block] = true;
                grid.blockOfCell[cell] = static_cast<std::uint8_t>(block);
                grid.cellOfBlock[block] = static_cast<std::uint8_t>(cell);
            }
            grid.hierarchical = ok;
        }
        return built;
    }();
    return grids[static_cast<std::uint32_t>(psm) & 63u];
}

std::uint64_t FieldOf(std::uint64_t value, unsigned shift, unsigned width) {
    return (value >> shift) & ((1ull << width) - 1ull);
}

// A rectangle the EE wrote into GS memory, and in which format (4.1).
struct Dirty {
    PixelRect rect;
    Psm psm = Psm::Ct32;
};

// 1.2: one target.
struct Target {
    bool alive = false;
    FormatClass cls = FormatClass::None;
    BufferView view{0, 0, Psm::Ct32};  // base, width and the format of its last draw
    ImageId image = kNoImage;
    std::uint32_t width = 0, height = 0;  // native size of its image
    PixelRect valid;                      // content that is defined
    PixelRect drawn;                      // drawn since the last readback
    std::vector<Dirty> dirty;             // newer in GS memory than in the image
    // IN-484: the blocks of the last rectangle asked for: CLUT loads ask for each target's blocks thousands of times per VBlank.
    mutable PixelRect spanRect;
    mutable BufferView spanView{0, 0, Psm::Ct32};
    mutable BlockSpan spanValue{};
    mutable bool spanKnown = false;
    bool fullWordWritten = false;         // a 32-bit (or Z32) draw happened: the top byte is the target's
    bool readOnGpu = false;               // IN-408: drawn pixels were read on the GPU (CLUT, GLM) without a readback
    std::uint64_t lastDraw = 0;
    std::uint64_t imageChanges = 0;       // IN-513: changes to the image that are not draws (patches)
    // IN-510: per page row of this target, when the GLM last got that row; valid for glmHeldDraw and glmHeldChanges.
    std::array<std::uint64_t, 64> glmRowWritten{};
    std::uint64_t glmHeldDraw = 0, glmHeldChanges = 0;
    std::uint64_t lastDrawVblank = 0;
    std::uint32_t unusedFrames = 0;
    bool usedThisFrame = false;
};

struct RecentUpload {
    BufferView view{0, 0, Psm::Ct32};
    PixelRect rect;
    BlockSpan span;
    std::uint64_t drawNumber = 0;
};

// A texture decoded from GS memory (3.7) in a 1x image of its own, valid while none of its pages were written.
struct TextureEntry {
    writeback::TextureKey key;
    BlockSpan span;
    std::uint64_t sequence = 0;
    ImageId image = kNoImage;
    std::uint64_t bytes = 0;
    std::uint64_t lastUse = 0;
    std::uint64_t content = 0;  // IN-453: the block key of the decoded blocks (0: unknown)
};

// IN-453: a 64-bit hash of one 256-byte GS memory block: four independent lanes of xor-multiply over its 32 words,
// then the lanes rotated together and an avalanche. A collision would reuse a wrong decode; at 64 bits that is far
// below every other risk in the renderer.
std::uint64_t HashBlock(const std::uint8_t *block) {
    constexpr std::uint64_t P0 = 0x9E3779B185EBCA87ull, P1 = 0xC2B2AE3D27D4EB4Full;
    std::uint64_t lane[4] = {0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull};
    for (std::uint32_t i = 0; i < 32; i += 4) {
        std::uint64_t w[4];
        std::memcpy(w, block + i * 8, 32);
        for (std::uint32_t l = 0; l < 4; ++l) lane[l] = (lane[l] ^ w[l]) * P0;
    }
    std::uint64_t h = lane[0] ^ ((lane[1] << 17) | (lane[1] >> 47)) ^ ((lane[2] << 31) | (lane[2] >> 33)) ^
                      ((lane[3] << 47) | (lane[3] >> 17));
    h ^= h >> 33;
    h *= P1;
    h ^= h >> 29;
    h *= P0;
    h ^= h >> 32;
    return h;
}

// IN-486: a 64-bit hash of an EE upload's bytes, the lanes of HashBlock over any length.
std::uint64_t HashUploadBytes(const std::uint8_t *data, std::size_t byteCount, std::uint64_t seed) {
    constexpr std::uint64_t P0 = 0x9E3779B185EBCA87ull, P1 = 0xC2B2AE3D27D4EB4Full;
    std::uint64_t lane[4] = {0x243F6A8885A308D3ull ^ seed, 0x13198A2E03707344ull, 0xA4093822299F31D0ull ^ byteCount,
                             0x082EFA98EC4E6C89ull};
    std::size_t at = 0;
    for (; at + 32u <= byteCount; at += 32u) {
        std::uint64_t w[4];
        std::memcpy(w, data + at, 32);
        for (std::uint32_t l = 0; l < 4; ++l) lane[l] = (lane[l] ^ w[l]) * P0;
    }
    std::uint64_t rest[4] = {};
    if (at < byteCount) std::memcpy(rest, data + at, byteCount - at);
    for (std::uint32_t l = 0; l < 4; ++l) lane[l] = (lane[l] ^ rest[l]) * P0;
    std::uint64_t h = lane[0] ^ ((lane[1] << 17) | (lane[1] >> 47)) ^ ((lane[2] << 31) | (lane[2] >> 33)) ^
                      ((lane[3] << 47) | (lane[3] >> 17));
    h ^= h >> 33;
    h *= P1;
    h ^= h >> 29;
    h *= P0;
    h ^= h >> 32;
    return h;
}

// IN-453: mixes one word into a 64-bit key.
std::uint64_t MixWords(std::uint64_t h, std::uint64_t w) {
    h ^= w + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xBF58476D1CE4E5B9ull;
}

// 1x texels kept on the GPU (IN-431: 192 MiB on an integrated GPU; GT4_CORE2_TEXTURE_MB overrides both).
constexpr std::uint64_t kTextureCacheBytes = static_cast<std::uint64_t>(768) << 20;
constexpr std::uint64_t kTextureCacheBytesIntegrated = static_cast<std::uint64_t>(192) << 20;

std::uint64_t KeyHash(const writeback::TextureKey &k) {
    const std::uint64_t words[] = {k.tbp, k.tbw, k.psm, k.width, k.height, k.ta0, k.ta1, k.aem, k.paletteHash, k.mipHash};
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint64_t w : words) {
        hash ^= w;
        hash *= 1099511628211ull;
    }
    return hash;
}

int RowFieldOfDisplayField(bool csrFieldOdd) { return csrFieldOdd ? 0 : 1; }

// CPU time of CORE2's stages (microseconds, summed between two [core2] lines).
struct PhaseTimes {
    double gif = 0, batch = 0, upload = 0, written = 0, clut = 0, clutLoads = 0, uploads = 0, hook = 0, glmHook = 0, translate = 0, targets = 0, texture = 0, draw = 0, readback = 0, patch = 0, compose = 0,
           flush = 0, cpuSprites = 0, spriteCheck = 0, glm = 0, glmPages = 0, glmCalls = 0, swizzle = 0, swizzleBytes = 0;
    // TASK-027 CPU survey: the VBlank work (Renderer::Flush) and LOCAL->HOST reads (Renderer::ReadFifo).
    double vblank = 0, fifo = 0;
};

class ScopedUs;

// TASK-027 CPU survey: each timed stage is kept inclusive (its whole duration, as before) and exclusive (minus the
// timed stages nested in it), so the exclusive times add up to the time spent inside any stage.
struct PhaseClock {
    PhaseTimes inclusive;
    PhaseTimes exclusive;
    ScopedUs *open = nullptr;  // the innermost running stage
};

// Microseconds per TSC tick, measured once against the QPC clock (the TSC rate is constant on the qualified CPUs);
// one TSC read costs a few nanoseconds, a fraction of a steady_clock read, and a VBlank times thousands of stages.
double MicrosecondsPerTick() {
    static const double value = [] {
        const auto start = std::chrono::steady_clock::now();
        const std::uint64_t startTicks = __rdtsc();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2)) {
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        return us / static_cast<double>(__rdtsc() - startTicks);
    }();
    return value;
}

// TASK-134: the stages are timed only when a report will show them (the launcher's performance report, or a bench): a player's
// session pays for no clock reads, about 4% of this thread's CPU in a race. The [core2] lines then read 0 for the stage times.
bool PhaseTimingOn() {
    static const bool on = std::getenv("GT4_PERF") || std::getenv("GT4_HW_PROFILE_EVERY") || std::getenv("GT4_MTVU_REPORT_EVERY");
    return on;
}

class ScopedUs {
public:
    ScopedUs(PhaseClock &clock, double PhaseTimes::*field)
        : clock_(clock), field_(field), parent_(clock.open), start_(0), stopped_(!PhaseTimingOn()) {
        if (stopped_) return;
        start_ = __rdtsc();
        clock.open = this;
    }
    ~ScopedUs() { Stop(); }
    // Ends the stage before the end of its scope (stages end in the reverse order they began).
    void Stop() {
        if (stopped_) return;
        stopped_ = true;
        const double us = static_cast<double>(__rdtsc() - start_) * MicrosecondsPerTick();
        clock_.inclusive.*field_ += us;
        clock_.exclusive.*field_ += us - children_;
        if (parent_) parent_->children_ += us;
        clock_.open = parent_;
    }
    ScopedUs(const ScopedUs &) = delete;
    ScopedUs &operator=(const ScopedUs &) = delete;

private:
    PhaseClock &clock_;
    double PhaseTimes::*field_;
    ScopedUs *parent_;
    std::uint64_t start_;
    double children_ = 0;
    bool stopped_;
};

}  // namespace

struct Renderer::Impl final : public BatchSink, public SyncSink, public TransferSink, public writeback::TextureCacheView {
    // ---- State ---------------------------------------------------------------------------------------------
    cpuref::LocalMemorySink memory_;
    FrontEnd frontEnd_;
    Gpu gpu_;
    std::uint32_t scale_ = 1;
    float shift_ = 1.0f;
    bool failed_ = false;
    std::string failure_;
    writeback::Counters counters;
    Core2Counters stats;
    PhaseClock clock_;
    PhaseTimes &times_ = clock_.inclusive;  // the inclusive stage times (the [core2] ms/vblank line)

    display::DisplayRegisters displayRegs_;
    std::array<std::uint32_t, 2> previousDby_{};
    std::array<std::uint8_t, 3> background_{};
    bool oddField_ = false;
    writeback::OutputSettings settings_;
    std::uint64_t vblanks_ = 0;
    bool fxaa_ = false;  // IN-405: 13.5 FXAA of the presented frame
    // IN-432: the presented frame is resampled to the window rectangle on the GPU (Lanczos-2 per axis when it
    // shrinks, FSR1 EASU when it grows), then FSR1 RCAS sharpening when asked. The presenter's own box halvings and
    // bilinear blit then copy it 1:1. GT4_CORE2_PRESENT_FILTER=bilinear keeps the presenter's blit (A/B);
    // GT4_CORE2_FSR_SHARPNESS=<stops> turns RCAS on (0 strongest, 0.2 FSR's usual, 2 mild);
    // GT4_CORE2_PRESENT_SIZE=<w>x<h> forces the rectangle (headless checks; the window sets it otherwise).
    std::uint32_t presentWidth_ = 0, presentHeight_ = 0;
    // IN-433 (C5): GT4_HW_FILTER=forced samples every texture bilinearly where the game asked for nearest (as the
    // reference renderer's forced bilinear): softer 1x textures magnified at scale (menu grids, map dashes, distant
    // signs). forced_no_sprite leaves sprites alone: GT4 draws its HUD, fonts and full-screen copies (tone pass, the
    // frame copies of its effects) as sprites, where bilinear only blurs. Off by default (faithful).
    std::uint32_t forcedBilinear_ = 0;
    // IN-434 (C5): GT4_CORE2_ANISO=2|4|8|16 spreads up to that many samples along the long axis of a mipmapped
    // texture's footprint (hw_pixel_uber.frag sample_mipmapped). Off by default (the GS has no anisotropy).
    const std::uint32_t anisoLog2_ = [] {
        const char *v = std::getenv("GT4_CORE2_ANISO");
        const int n = v ? std::atoi(v) : 0;
        return n >= 16 ? 4u : n >= 8 ? 3u : n >= 4 ? 2u : n >= 2 ? 1u : 0u;
    }();
    const bool presentResample_ = std::getenv("GT4_CORE2_PRESENT_FILTER") == nullptr ||
                                  std::strcmp(std::getenv("GT4_CORE2_PRESENT_FILTER"), "bilinear") != 0;
    const float rcasSharpness_ = std::getenv("GT4_CORE2_FSR_SHARPNESS") != nullptr
                                     ? std::exp2(-std::max(0.0f, static_cast<float>(std::atof(std::getenv("GT4_CORE2_FSR_SHARPNESS")))))
                                     : 0.0f;
    // IN-405: the GPU deinterlace history (the old core's rules, IN-381/IN-386/IN-396): which of the four merge images
    // holds which field, by age.
    struct DeinterlaceHistory {
        std::uint32_t width = 0, height = 0;
        std::array<bool, gpu::kGpuDeinterlaceHistory> valid{};
        std::array<int, gpu::kGpuDeinterlaceHistory> parity{};
        std::array<std::uint64_t, gpu::kGpuDeinterlaceHistory> age{};
        std::uint64_t clock = 0;
        bool woven = false;
        bool output = false;
        void Clear() {
            valid.fill(false);
            woven = false;
            output = false;
        }
    };
    DeinterlaceHistory deinterlace_;
    // A second compose in the same VBlank (a dump next to the present) returns the first, so the history moves once.
    std::uint64_t composedVblank_ = ~std::uint64_t{0};
    writeback::GpuFrame lastFrame_;

    std::vector<Target> targets_;
    std::uint64_t drawCounter_ = 0;
    std::deque<RecentUpload> recentUploads_;

    std::array<std::uint64_t, memory::kPagesTotal> pageWrites_{};
    // IN-453: an identity per GS memory block (4 KiB pages were the unit before: a texture went stale whenever any
    // other block of its pages was written, GT4's uploads of the textures beside it). EE uploads give the blocks they
    // change the hash of their bytes (SetUploadIds); every other write (readbacks, moves, clears) gives the blocks of
    // its span fresh unique identities. A texture's block key (its address key and its blocks' identities) names its
    // bytes, so an unchanged key is an unchanged decode.
    std::vector<std::uint64_t> blockIds_ = [] {
        std::vector<std::uint64_t> ids(kAllBlocks);
        for (std::uint32_t i = 0; i < kAllBlocks; ++i) ids[i] = i + 1u;
        return ids;
    }();
    std::uint64_t nextBlockId_ = kAllBlocks + 1u;  // never repeated: unique identities count up from here
    std::vector<std::uint64_t> changedBlocks_ = std::vector<std::uint64_t>(kAllBlocks / 64u);  // set by the swizzle
    std::unordered_map<std::uint64_t, std::vector<std::uint16_t>> blockLists_;  // the blocks of a texture address key
    // IN-453: GT4_CORE2_BLOCK_VERSIONS=0 keeps the page versions alone for the texture cache (A/B).
    const bool contentCacheOn_ = std::getenv("GT4_CORE2_BLOCK_VERSIONS") == nullptr ||
                                 std::strcmp(std::getenv("GT4_CORE2_BLOCK_VERSIONS"), "0") != 0;
    std::array<std::uint8_t, memory::kPagesTotal> pageWriter_{};  // IN-450: WriteKind of the newest write
    std::uint64_t writeSequence_ = 0;
    std::unordered_map<std::uint64_t, TextureEntry> textures_;
    std::unordered_map<std::uint64_t, std::vector<ImageId>> texturePool_;  // free 1x images by size
    // The GLM (GPU copy of GS memory) holds a page as of glmSequence_[page]; never uploaded pages are stale.
    std::array<std::uint64_t, memory::kPagesTotal> glmSequence_{};
    std::array<bool, memory::kPagesTotal> glmUploaded_{};
    std::uint64_t textureBytes_ = 0;
    std::uint64_t textureBudget_ = kTextureCacheBytes;  // IN-431: set once the GPU is known (Renderer::Init)
    std::uint64_t textureUses_ = 0;
    // Set by find() for the batch being translated: the image of a cached texture.
    ImageId textureHit_ = kNoImage;
    // Set by the texture-from-target hook for the batch being translated.
    std::int32_t textureTarget_ = -1;
    PixelRect textureFootprint_{};  // the footprint the target texture hook was asked for (spec 8.8 copies)
    // Spec 8.8 copies of a target the batch draws into (OnBatch); GT4_CORE2_SELF_COPY=0 samples the target itself.
    const bool selfCopy_ =
        std::getenv("GT4_CORE2_SELF_COPY") == nullptr || std::strcmp(std::getenv("GT4_CORE2_SELF_COPY"), "0") != 0;

    // 7.2: the channel-shuffle run in progress.
    bool toneActive_ = false;
    std::uint32_t toneFbmsk_ = 0;

    std::optional<LocalToHostSetup> pendingRead_;

    // IN-439 (performance mode, decision 0073; GT4_CORE2_PERFORMANCE=1, or GT4_CORE2_DELAYED_DOWNLOADS=1 alone): an EE
    // download of a rectangle inside one target with no pending dirty rectangles is answered with the words of the
    // previous download of the same rectangle, reduced on the GPU without a wait; this one's reduction is taken next
    // time. GT4 downloads one tiny row per VBlank in a race (its light flare visibility test, the same buffer for 10-25
    // VBlanks in a row), and each download made the CPU wait for all the GPU work so far: at 10x, 3 ms of waiting
    // per VBlank and the GPU idle afterwards. The answer is one download (about one VBlank) late; the first download
    // of a rectangle, and any other download, is read back as before (exact).
    struct DelayedDownload {
        std::vector<std::uint32_t> words;  // the last result taken (empty: none yet)
        bool pending = false;              // a reduction in flight under `ticket`
        std::uint32_t ticket = 0;
    };
    std::map<std::array<std::uint32_t, 7>, DelayedDownload> delayed_;
    // IN-497: GT4_CORE2_EFFECTS_1X=0 or 1 overrides the default, on in Performance Mode (see CreateTarget).
    const bool effectTargets1x_ = std::getenv("GT4_CORE2_EFFECTS_1X") != nullptr
                                      ? std::strcmp(std::getenv("GT4_CORE2_EFFECTS_1X"), "0") != 0
                                      : std::getenv("GT4_CORE2_PERFORMANCE") != nullptr && std::strcmp(std::getenv("GT4_CORE2_PERFORMANCE"), "0") != 0;
    const bool performance_ = std::getenv("GT4_CORE2_PERFORMANCE") != nullptr &&
                              std::strcmp(std::getenv("GT4_CORE2_PERFORMANCE"), "0") != 0;
    const bool delayedDownloads_ = std::getenv("GT4_CORE2_DELAYED_DOWNLOADS") != nullptr
                                       ? std::strcmp(std::getenv("GT4_CORE2_DELAYED_DOWNLOADS"), "0") != 0
                                       : performance_;
    // IN-441 (performance mode, as rc7's TASK-117 does for the PCSX2 GS): GT4's night race reads back its light flare
    // visibility table, a 64 x 8 PSMCT32 rectangle (2048 bytes), once per frame; the VU1 scales every light flare and the
    // sun glare by its entry. Answered as zeros, without a readback, the game draws none of them: no GPU wait for the
    // table and no flare draws. GT4_GS_HIDE_FLARES=0/1 overrides the performance mode default (the same switch as rc7's).
    const bool hideFlares_ = std::getenv("GT4_GS_HIDE_FLARES") != nullptr
                                 ? std::strcmp(std::getenv("GT4_GS_HIDE_FLARES"), "0") != 0
                                 : performance_;
    bool zeroRead_ = false;  // the pending LOCAL->HOST read is a hidden flare table: its bytes are zeros
    const char *readbackReason_ = "other";  // diagnostics (GT4_CORE2_READBACK_TRACE): why the next readback runs
    // B1: GPU fence wait and readback count per readback reason, between two [core2] lines.
    struct ReasonWait {
        const char *reason;
        std::uint64_t waitNs = 0;
        std::uint64_t readbacks = 0;
    };
    std::array<ReasonWait, 5> reasonWaits_{{{"download"}, {"before write"}, {"keep"}, {"clut"}, {"other"}}};
    ReasonWait &WaitOf(const char *reason) {
        for (ReasonWait &w : reasonWaits_) {
            if (std::strcmp(w.reason, reason) == 0) return w;
        }
        return reasonWaits_.back();
    }

    // IN-408: CLUT loads from drawn targets on the GPU. GT4 draws and filters palettes on the GPU (night races: several
    // per VBlank) and loads a CLUT from them right away; a readback of each was a submit and a full wait. Such a load
    // is now read on the GPU from the target into the GPU copy of the CLUT buffer, and the CPU CLUT unit, which
    // still decides when a load happens, keeps the stale words it read from memory. clutSource_ says, per halfword,
    // which GPU load wrote it (0: the CPU CLUT unit holds it). From the first such load on, every load is mirrored
    // into the GPU CLUT, so it always equals the buffer the GS would hold. An indexed texture that uses a GPU-written
    // halfword is expanded on the GPU; a CPU user of such a palette (the tone hook) reads the GPU CLUT back first.
    // GT4_CORE2_GPU_CLUT=0 keeps the readback (A/B).
    // Diagnostics: GT4_CORE2_GPU_CLUT=clut keeps only the CLUT loads on the GPU, =textures only the texture reads.
    const char *gpuClutMode_ = std::getenv("GT4_CORE2_GPU_CLUT") ? std::getenv("GT4_CORE2_GPU_CLUT") : "1";
    const bool gpuClut_ = std::strcmp(gpuClutMode_, "0") != 0 && std::strcmp(gpuClutMode_, "textures") != 0;
    const bool gpuTextures_ = std::strcmp(gpuClutMode_, "0") != 0 && std::strcmp(gpuClutMode_, "clut") != 0;
    std::array<std::uint64_t, 512> clutSource_{};
    std::uint64_t clutTargetLoadCount_ = 0;
    bool gpuClutMirror_ = false;
    // Halfwords loaded from memory since the GPU CLUT last received the CPU unit's words: written to the GPU only
    // right before something on the GPU reads the GPU CLUT (GT4 loads about 500 CLUTs per night VBlank, almost all
    // from memory; mirroring each one was a copy and a barrier).
    std::array<bool, 512> clutMirrorPending_{};
    bool clutMirrorAnyPending_ = false;
    bool clutFromTargetPending_ = false;
    std::uint64_t lastWaitNs_ = 0;
    // S1: per-VBlank spike log. A VBlank whose GS thread time or wall time passes GT4_CORE2_SPIKE_MS (default 20;
    // 0 = off) prints what it did, at most GT4_CORE2_SPIKE_LINES (default 40) lines per 600 VBlanks.
    struct SpikeMark {
        std::chrono::steady_clock::time_point at{};
        double gif = 0, upload = 0, texture = 0, readback = 0, translate = 0, draw = 0, targets = 0, clut = 0, patch = 0;
        double uploads = 0, swizzleBytes = 0;
        std::uint64_t createdImages = 0, gpuNs = 0, fragments = 0, prims = 0;
        std::uint64_t waitNs = 0, readbacks = 0, downloads = 0, stores = 0, keeps = 0, created = 0, allocated = 0,
                      decodes = 0, submits = 0, materializations = 0, dirty = 0;
    };
    SpikeMark spikeMark_;
    const long long profileAt_ =
        std::getenv("GT4_CORE2_GPU_PROFILE_AT") ? std::atoll(std::getenv("GT4_CORE2_GPU_PROFILE_AT")) : -1;
    std::uint32_t spikeLines_ = 0;
    const double spikeMs_ = std::getenv("GT4_CORE2_SPIKE_MS") ? std::atof(std::getenv("GT4_CORE2_SPIKE_MS")) : 20.0;
    const std::uint32_t spikeLineLimit_ = std::getenv("GT4_CORE2_SPIKE_LINES") ? static_cast<std::uint32_t>(std::atoi(std::getenv("GT4_CORE2_SPIKE_LINES"))) : 40u;
    SpikeMark TakeSpikeMark() const {
        SpikeMark m;
        m.at = std::chrono::steady_clock::now();
        m.gif = times_.gif;
        m.upload = times_.upload;
        m.texture = times_.texture;
        m.readback = times_.readback;
        m.translate = times_.translate;
        m.draw = times_.draw;
        m.targets = times_.targets;
        m.clut = times_.clut;
        m.patch = times_.patch;
        m.uploads = times_.uploads;
        m.swizzleBytes = times_.swizzleBytes;
        m.waitNs = gpu_.Stats().waitNs;
        m.readbacks = stats.readbacks;
        m.downloads = stats.downloadReadbacks;
        m.stores = stats.storesBeforeWrite;
        m.keeps = stats.gpuReadKeeps;
        m.created = stats.targetsCreated;
        m.allocated = gpu_.Stats().imagesAllocated + gpu_.Stats().imageBlocks;  // device allocations
        m.createdImages = gpu_.Stats().imagesCreated;
        m.gpuNs = gpu_.Stats().gpuNs;  // finished submissions: lags the VBlank by a submission or two
        m.fragments = gpu_.Stats().fragments;
        m.prims = gpu_.Stats().primitives;
        m.decodes = gpu_.Stats().atlasUploads;
        m.submits = gpu_.Stats().submissions;
        m.materializations = stats.materializations;
        m.dirty = stats.dirtyPatches;
        return m;
    }
    void NoteSpike() {
        const SpikeMark now = TakeSpikeMark();
        if (spikeMs_ > 0.0 && spikeMark_.at.time_since_epoch().count() != 0 && spikeLines_ < spikeLineLimit_) {
            const SpikeMark &b = spikeMark_;
            const double wall = std::chrono::duration<double, std::milli>(now.at - b.at).count();
            const double gs = (now.gif - b.gif) / 1000.0;
            if (gs >= spikeMs_ || wall >= spikeMs_) {
                spikeLines_++;
                std::fprintf(stderr,
                             "[core2] spike vblank=%llu wall=%.1f gs=%.1f upload=%.1f (%.0f, %.0f KiB) texture=%.1f "
                             "readback=%.1f translate=%.1f draw=%.1f targets=%.1f clut=%.1f patch=%.1f gpu_wait=%.1f "
                             "readbacks=%llu (download %llu, store %llu, keep %llu) created=%llu images=%llu allocated=%llu "
                             "decodes=%llu submits=%llu glm_writes=%llu dirty=%llu gpu_busy=%.1f fragments=%llu prims=%llu\n",
                             static_cast<unsigned long long>(vblanks_), wall, gs, (now.upload - b.upload) / 1000.0,
                             now.uploads - b.uploads, (now.swizzleBytes - b.swizzleBytes) / 1024.0,
                             (now.texture - b.texture) / 1000.0, (now.readback - b.readback) / 1000.0,
                             (now.translate - b.translate) / 1000.0, (now.draw - b.draw) / 1000.0,
                             (now.targets - b.targets) / 1000.0, (now.clut - b.clut) / 1000.0,
                             (now.patch - b.patch) / 1000.0, static_cast<double>(now.waitNs - b.waitNs) / 1.0e6,
                             static_cast<unsigned long long>(now.readbacks - b.readbacks),
                             static_cast<unsigned long long>(now.downloads - b.downloads),
                             static_cast<unsigned long long>(now.stores - b.stores),
                             static_cast<unsigned long long>(now.keeps - b.keeps),
                             static_cast<unsigned long long>(now.created - b.created),
                             static_cast<unsigned long long>(now.createdImages - b.createdImages),
                             static_cast<unsigned long long>(now.allocated - b.allocated),
                             static_cast<unsigned long long>(now.decodes - b.decodes),
                             static_cast<unsigned long long>(now.submits - b.submits),
                             static_cast<unsigned long long>(now.materializations - b.materializations),
                             static_cast<unsigned long long>(now.dirty - b.dirty),
                             static_cast<double>(now.gpuNs - b.gpuNs) / 1.0e6,
                             static_cast<unsigned long long>(now.fragments - b.fragments),
                             static_cast<unsigned long long>(now.prims - b.prims));
            }
        }
        spikeMark_ = now;
    }
    std::uint64_t lastGpuNs_ = 0, lastTimedSubmissions_ = 0, lastDrawPassNs_ = 0;
    std::uint64_t lastPrimitives_ = 0, lastFragments_ = 0, lastCompute_ = 0;  // B4: pipeline statistics  // B4: GPU time at the last timing line
    std::array<std::uint64_t, static_cast<std::size_t>(FlushReason::Count)> lastFlushCounts_{};  // CPU survey R9
    std::uint64_t lastClutSameTexture_ = 0;

    // IN-409: indexed memory textures keep their CLUT indices (cached whatever the palette) and draws look the colour
    // up in a row of the palette image: one row per distinct palette (content hash for CPU palettes, the GPU loads
    // that wrote it for GPU ones), reused round-robin. GT4_CORE2_PALETTE_ROWS=0 expands palettes into every decode.
    const bool paletteRows_ = std::getenv("GT4_CORE2_PALETTE_ROWS") == nullptr ||
                              std::strcmp(std::getenv("GT4_CORE2_PALETTE_ROWS"), "0") != 0;
    static constexpr std::uint32_t kPaletteRows = 1024;
    ImageId paletteImage_ = kNoImage;
    std::unordered_map<std::uint64_t, std::uint32_t> paletteRowOf_;
    std::vector<std::uint64_t> paletteRowKey_;
    std::uint32_t nextPaletteRow_ = 0;

    // The palette row of an indexed texture's palette (TEX0 CPSM and CSA, TEXA), filled on first use; -1 on failure.
    std::int32_t PaletteRow(const writeback::BatchTranslation &t, std::uint64_t tex0, std::uint64_t texa) {
        const std::uint32_t cpsm = static_cast<std::uint32_t>(tex0::Cpsm(tex0));
        const std::uint32_t csa = static_cast<std::uint32_t>(tex0::Csa(tex0));
        const std::uint64_t gpuKey = GpuPaletteKey(t.glmPsm, cpsm, csa);
        const std::uint64_t entries = (t.glmPsm & 7u) == 3u ? 256u : 16u;
        std::uint64_t key = gpuKey != 0 ? gpuKey : writeback::HashPaletteWords(t.palette);
        const std::uint64_t parts[] = {entries, gpuKey != 0 ? cpsm : 0u, gpuKey != 0 ? csa : 0u,
                                       gpuKey != 0 ? (texa & 0xFF000080FFull) : 0u};
        for (std::uint64_t part : parts) key = (key ^ part) * 1099511628211ull;
        if (auto found = paletteRowOf_.find(key); found != paletteRowOf_.end()) {
            stats.paletteRowHits++;
            return static_cast<std::int32_t>(found->second);
        }
        std::string error;
        if (paletteImage_ == kNoImage) {
            paletteImage_ = gpu_.CreateImage(256, kPaletteRows, false, error);
            if (paletteImage_ == kNoImage) {
                Fail(error);
                return -1;
            }
            paletteRowKey_.assign(kPaletteRows, 0);
        }
        const std::uint32_t row = nextPaletteRow_;
        nextPaletteRow_ = (nextPaletteRow_ + 1u) % kPaletteRows;
        if (paletteRowKey_[row] != 0) paletteRowOf_.erase(paletteRowKey_[row]);
        paletteRowKey_[row] = key;
        paletteRowOf_[key] = row;
        bool ok = false;
        if (gpuKey != 0) {
            if (!FlushClutMirror()) return -1;
            GpuPalette palette;
            palette.cpsm = cpsm;
            palette.csa = csa;
            palette.texa = texa;
            ok = gpu_.PaletteRowFromGpuClut(paletteImage_, row, t.glmPsm, palette, error);
        } else {
            std::vector<std::uint32_t> words(t.palette.begin(), t.palette.end());
            words.resize(256, 0u);
            ok = gpu_.Patch(paletteImage_, 0, static_cast<std::int32_t>(row), 255, static_cast<std::int32_t>(row), words,
                            0xFFFFFFFFu, error);
        }
        if (!ok) {
            Fail(error);
            return -1;
        }
        stats.paletteRowsWritten++;
        return static_cast<std::int32_t>(row);
    }  // B0: GPU fence wait time at the last timing line

    std::vector<gpu::GpuVertex> vertexScratch_;
    std::vector<Qword> gifScratch_;

    static FrontEndOptions Core2FrontEndOptions() {
        FrontEndOptions options;
        options.vertexTrace = false;
        return options;
    }

    Impl() : frontEnd_(Core2FrontEndOptions(), *this, *this, *this) {
        frontEnd_.SetLocalMemory(memory_.Local().bytes().data());
        // IN-485: remembered palettes (A/B).
        const char *clutMemo = std::getenv("GT4_CORE2_CLUT_MEMO");
        frontEnd_.MutableClut().SetDataMemo(clutMemo == nullptr || std::strcmp(clutMemo, "0") != 0);
        // A CLUT load reads GS memory. GT4 draws some of its palettes (the race colour filter's ramp), so a load over
        // a drawn target reduces that target into memory first, like any host read of drawn pixels (4.2).
        frontEnd_.SetBeforeLocalRead([this](const BlockRange &range) {
            ScopedUs clutTimer(clock_, &PhaseTimes::clut);
            times_.clutLoads += 1;
            const BlockSpan span{range.begin, range.count};
            // IN-408: one drawn 32-bit colour target under a CSM1 palette is read on the GPU when the load happens.
            if (gpuClut_ && span.count < kAllBlocks && (ClutSourceTarget(span) != nullptr || ClutTargetsOnGpu(span))) {
                clutFromTargetPending_ = true;
                return;
            }
            for (const Target &t : targets_) {
                if (t.alive && !t.drawn.Empty() && SpansOverlap(TargetSpan(t, t.drawn), span)) {
                    stats.clutReadbacks++;
                    readbackReason_ = "clut";
                    Readback(span);
                    readbackReason_ = "other";
                    return;
                }
            }
        });
        frontEnd_.SetClutLoaded([this](const clut::Tex0Write &write) {
            ScopedUs clutTimer(clock_, &PhaseTimes::clut);
            OnClutLoaded(write);
        });
    }

    // IN-408: the drawn target a CSM1 palette over `span` must be read from on the GPU: exactly one drawn target
    // overlaps it and it is a 32-bit colour target; nullptr otherwise (the readback path).
    Target *ClutSourceTarget(const BlockSpan &span) {
        Target *found = nullptr;
        for (Target &t : targets_) {
            if (!t.alive || t.drawn.Empty() || !SpansOverlap(TargetSpan(t, t.drawn), span)) continue;
            if (found || t.cls != FormatClass::Colour32) return nullptr;
            found = &t;
        }
        return found;
    }

    // IN-417: GT4_CORE2_GPU_MULTI_TARGET=0 keeps depth targets out of the GLM writes and reads CLUT loads over several
    // drawn targets back on the CPU (A/B).
    const bool multiTarget_ = std::getenv("GT4_CORE2_GPU_MULTI_TARGET") == nullptr ||
                              std::strcmp(std::getenv("GT4_CORE2_GPU_MULTI_TARGET"), "0") != 0;
    // IN-483: GT4 loads palettes over 64 x 64 CT16 targets in its races: 16-bit targets too, or each load makes the CPU wait
    // for the GPU (2 ms per VBlank on the HD 620). GT4_CORE2_GPU_CT16=0 reads them back (A/B).
    const bool gpuColour16_ = std::getenv("GT4_CORE2_GPU_CT16") == nullptr || std::strcmp(std::getenv("GT4_CORE2_GPU_CT16"), "0") != 0;
    bool MaterializableClass(FormatClass cls) const {
        return cls == FormatClass::Colour32 || (multiTarget_ && cls == FormatClass::Depth32) ||
               (multiTarget_ && gpuColour16_ && cls == FormatClass::Colour16);
    }

    // IN-417: whether a CLUT load over `span` can be read on the GPU from the GLM after the drawn targets under it are
    // written there: at least one drawn target overlaps it and every one is a 32-bit colour or depth target. GT4 loads
    // palettes from the memory of its 128 x 128 night render, which also holds a Z24 target and lies inside its
    // 640 x 224 frames.
    bool ClutTargetsOnGpu(const BlockSpan &span) const {
        if (!multiTarget_) return false;
        bool any = false;
        for (const Target &t : targets_) {
            if (!t.alive || t.drawn.Empty() || !SpansOverlap(TargetSpan(t, t.drawn), span)) continue;
            if (!MaterializableClass(t.cls)) return false;
            any = true;
        }
        return any;
    }

    // B4: a list of CLUT buffer halfwords (at most 512) that needs no allocation; about 500 CLUT loads per night VBlank
    // build one each.
    struct HalfwordList {
        std::array<std::uint16_t, 512> index;
        std::uint32_t count = 0;
        void push_back(std::uint32_t value) {
            if (count < index.size()) index[count++] = static_cast<std::uint16_t>(value);
        }
        bool empty() const { return count == 0; }
        const std::uint16_t *begin() const { return index.data(); }
        const std::uint16_t *end() const { return index.data() + count; }
    };

    // IN-457: the CLUT buffer halfwords a load writes (hw_glm_clut.comp, ClutUnit::Copy) are at most two ranges
    // [begin, end): PSMCT32 palette group g goes to buffer group g + CSA (groups past 15 are not loaded), its low halves
    // at 0..255 and its high halves at 256..511; PSMCT16 entry e goes to (16 * CSA + e) mod 512, so a range that wraps.
    struct HalfwordRanges {
        std::uint32_t begin[2] = {0, 0};
        std::uint32_t end[2] = {0, 0};
        bool empty() const { return begin[0] == end[0] && begin[1] == end[1]; }
        template <typename Array, typename Value>
        void Fill(Array &array, Value value) const {
            for (int r = 0; r < 2; ++r) std::fill(array.begin() + begin[r], array.begin() + end[r], value);
        }
    };
    static HalfwordRanges ClutHalfwordsWritten(const clut::Tex0Write &write) {
        HalfwordRanges out;
        const std::uint32_t count = (write.psm & 7u) == 3u ? 256u : 16u;
        if (write.cpsm == 0) {
            const std::uint32_t firstGroup = write.csa & 15u;
            const std::uint32_t endGroup = std::min(16u, firstGroup + count / 16u);
            out.begin[0] = 16u * firstGroup;
            out.end[0] = 16u * endGroup;
            out.begin[1] = 256u + 16u * firstGroup;
            out.end[1] = 256u + 16u * endGroup;
        } else {
            const std::uint32_t start = (16u * write.csa) & 511u;
            out.begin[0] = start;
            out.end[0] = std::min(512u, start + count);
            out.end[1] = start + count > 512u ? start + count - 512u : 0u;
        }
        return out;
    }

    // The CLUT buffer halfwords an indexed texture's palette reads (hw_glm_decode.comp gpu_palette_entry).
    static HalfwordList ClutHalfwordsUsed(std::uint32_t psm, std::uint32_t cpsm, std::uint32_t csa) {
        HalfwordList out;
        const std::uint32_t count = (psm & 7u) == 3u ? 256u : 16u;
        for (std::uint32_t e = 0; e < count; ++e) {
            if (cpsm == 0) {
                const std::uint32_t group = std::min((e >> 4) + (csa & 15u), 15u);
                out.push_back(16u * group + (e & 15u));
                out.push_back(16u * group + (e & 15u) + 256u);
            } else {
                out.push_back((16u * csa + e) & 511u);
            }
        }
        return out;
    }

    // IN-408: a hash of which GPU loads wrote the halfwords a palette uses; 0 when the CPU CLUT unit holds them all.
    std::uint64_t GpuPaletteKey(std::uint32_t psm, std::uint32_t cpsm, std::uint32_t csa) const {
        std::uint64_t hash = 0;
        bool gpu = false;
        for (std::uint32_t index : ClutHalfwordsUsed(psm, cpsm, csa)) {
            if (clutSource_[index] == 0) continue;
            gpu = true;
            hash = (hash ^ (clutSource_[index] * 0x9E3779B97F4A7C15ull + index)) * 1099511628211ull;
        }
        return gpu ? (hash | (1ull << 63)) : 0;
    }

    void OnClutLoaded(const clut::Tex0Write &write) {
        const bool fromTarget = clutFromTargetPending_;
        clutFromTargetPending_ = false;
        const HalfwordRanges written = ClutHalfwordsWritten(write);
        std::string error;
        if (fromTarget) {
            // An upload committed before the copy may have replaced the target: look again.
            const BlockSpan span{write.cbp & ~31u, 64u};
            if (Target *target = ClutSourceTarget(span)) {
                if (!gpuClutMirror_) {
                    // The GPU CLUT starts as the CPU unit's buffer (every halfword correct but the ones this load writes).
                    if (!gpu_.WriteClut(0, 512, frontEnd_.Clut().Halfwords(), error)) {
                        Fail(error);
                        return;
                    }
                    gpuClutMirror_ = true;
                }
                if (!FlushClutMirror() || !ApplyDirty(*target) || !MakeGlmCurrent(span)) return;
                ClutTargetLoad load;
                load.cbp = write.cbp;
                load.cpsm = write.cpsm;
                load.csm = write.csm;
                load.csa = write.csa;
                load.eightBit = (write.psm & 7u) == 3u;
                load.cbw = write.texclutCbw;
                load.cou = write.texclutCou;
                load.cov = write.texclutCov;
                load.tbp = target->view.basePointer;
                load.tbw = target->view.bufferWidth;
                const PixelRect held = Union(target->valid, target->drawn);
                load.heldX0 = held.x0;
                load.heldY0 = held.y0;
                load.heldX1 = held.x1;
                load.heldY1 = held.y1;
                load.fullWord = target->fullWordWritten;
                if (!gpu_.ClutFromTarget(target->image, load, error)) {
                    Fail(error);
                    return;
                }
                target->readOnGpu = true;  // not a use for 6.2 ageing (see MaterializeTargets)
                static const bool trace = std::getenv("GT4_CORE2_READBACK_TRACE") != nullptr;  // diagnostics only
                if (trace) {
                    std::fprintf(stderr, "[core2] gpu clut vblank=%llu cbp=%u psm=%u cpsm=%u csa=%u target bp=%u bw=%u "
                                 "held=%d,%d,%d,%d\n", static_cast<unsigned long long>(vblanks_), write.cbp, write.psm,
                                 write.cpsm, write.csa, target->view.basePointer, target->view.bufferWidth, held.x0,
                                 held.y0, held.x1, held.y1);
                }
                ++clutTargetLoadCount_;
                written.Fill(clutSource_, clutTargetLoadCount_);
                stats.clutTargetLoads++;
                return;
            }
            if (ClutTargetsOnGpu(span)) {
                // IN-417: several drawn targets (or a depth target): written into the GLM oldest draw first, then the
                // load reads the GLM alone (an empty held area).
                if (!gpuClutMirror_) {
                    if (!gpu_.WriteClut(0, 512, frontEnd_.Clut().Halfwords(), error)) {
                        Fail(error);
                        return;
                    }
                    gpuClutMirror_ = true;
                }
                static const bool multiTrace = std::getenv("GT4_CORE2_READBACK_TRACE") != nullptr;  // diagnostics only
                if (multiTrace) {
                    std::fprintf(stderr, "[core2] multi clut vblank=%llu cbp=%u psm=%u cpsm=%u csm=%u csa=%u\n",
                                 static_cast<unsigned long long>(vblanks_), write.cbp, write.psm, write.cpsm, write.csm, write.csa);
                }
                bool any = false;
                if (!FlushClutMirror() || !MakeGlmCurrent(span) || !MaterializeTargets(span, any)) return;
                const Target *bound = nullptr;  // any live image fills the target binding, which is not read
                for (const Target &t : targets_) {
                    if (t.alive && t.image != kNoImage) {
                        bound = &t;
                        break;
                    }
                }
                if (bound) {
                    ClutTargetLoad load;
                    load.cbp = write.cbp;
                    load.cpsm = write.cpsm;
                    load.csm = write.csm;
                    load.csa = write.csa;
                    load.eightBit = (write.psm & 7u) == 3u;
                    load.cbw = write.texclutCbw;
                    load.cou = write.texclutCou;
                    load.cov = write.texclutCov;
                    load.tbp = bound->view.basePointer;
                    load.tbw = bound->view.bufferWidth;
                    load.heldX0 = 0;
                    load.heldY0 = 0;
                    load.heldX1 = -1;
                    load.heldY1 = -1;
                    load.fullWord = true;
                    if (!gpu_.ClutFromTarget(bound->image, load, error)) {
                        Fail(error);
                        return;
                    }
                    ++clutTargetLoadCount_;
                    written.Fill(clutSource_, clutTargetLoadCount_);
                    stats.clutTargetLoads++;
                    stats.clutMultiTargetLoads++;
                    return;
                }
            }
        }
        // A load from memory: the CPU CLUT unit's words are right; the GPU CLUT gets them before its next reader.
        written.Fill(clutSource_, std::uint64_t{0});
        if (!gpuClutMirror_) return;
        written.Fill(clutMirrorPending_, true);
        clutMirrorAnyPending_ = clutMirrorAnyPending_ || !written.empty();
    }

    // Records the pending CPU halfwords into the GPU CLUT (contiguous runs), before a GPU reader of it.
    bool FlushClutMirror() {
        if (!clutMirrorAnyPending_) return true;
        const std::uint16_t *halfwords = frontEnd_.Clut().Halfwords();
        std::string error;
        for (std::uint32_t i = 0; i < 512;) {
            if (!clutMirrorPending_[i]) {
                ++i;
                continue;
            }
            std::uint32_t j = i;
            while (j < 512 && clutMirrorPending_[j]) clutMirrorPending_[j++] = false;
            if (!gpu_.WriteClut(i, j - i, halfwords, error)) {
                Fail(error);
                return false;
            }
            i = j;
        }
        clutMirrorAnyPending_ = false;
        return true;
    }

    // IN-408: a CPU user of the CLUT (the tone hook) gets the GPU CLUT's words when the GPU wrote any it reads.
    bool SyncClutToCpu(std::uint32_t psm, std::uint32_t cpsm, std::uint32_t csa) {
        if (GpuPaletteKey(psm, cpsm, csa) == 0) return true;
        if (!FlushClutMirror()) return false;
        std::uint16_t halfwords[512];
        std::string error;
        if (!gpu_.ReadClut(halfwords, error)) {
            Fail(error);
            return false;
        }
        frontEnd_.MutableClut().SetHalfwords(halfwords);
        clutSource_.fill(0);
        stats.clutReads++;
        return true;
    }

    void Fail(const std::string &error) {
        if (failed_) return;
        failed_ = true;
        failure_ = error;
        std::fprintf(stderr, "[core2] failure: %s\n", error.c_str());
    }

    // ---- GS memory page versions (texture cache, 3.7) --------------------------------------------------------
    // IN-450: who wrote a page last, so a stale texture decode can be attributed (the stale_by counters).
    enum WriteKind : std::uint8_t { kWriteUpload, kWriteReadback, kWriteDelayed, kWriteMove, kWriteWhole, kWriteKinds };
    void MarkWritten(const BlockSpan &span, WriteKind kind = kWriteWhole) {
        if (span.count == 0) return;
        ++writeSequence_;
        if (kind != kWriteUpload) FreshBlockIds(span);  // IN-453 (uploads call SetUploadIds with their exact blocks)
        if (kind == kWriteReadback) AuditIds("after fresh readback");
        if (kind == kWriteDelayed) AuditIds("after fresh delayed");
        if (kind == kWriteMove) AuditIds("after fresh move");
        if (kind == kWriteWhole) AuditIds("after fresh whole");
        if (span.count >= kAllBlocks) {
            pageWrites_.fill(writeSequence_);
            pageWriter_.fill(kind);
            return;
        }
        const std::uint32_t firstPage = span.begin / memory::kBlocksPerPage;
        const std::uint32_t lastBlock = span.begin + span.count - 1u;
        for (std::uint32_t page = firstPage; page <= lastBlock / memory::kBlocksPerPage; ++page) {
            pageWrites_[page % memory::kPagesTotal] = writeSequence_;
            pageWriter_[page % memory::kPagesTotal] = kind;
        }
    }

    // IN-453: unique identities for every block of `span` (contents nobody can name).
    void FreshBlockIds(const BlockSpan &span) {
        const std::uint32_t count = std::min<std::uint32_t>(span.count, kAllBlocks);
        for (std::uint32_t i = 0; i < count; ++i) blockIds_[(span.begin + i) % kAllBlocks] = nextBlockId_++;
    }

    // IN-453: an EE upload of `rect` in `view`: the block cells it touches (exactly them: the span of the write would
    // also take the blocks between them, other textures of the same page rows) get the hash of their 256 bytes as
    // their identity, so any history that leaves a block with the same bytes gives it the same identity (GT4 sends
    // textures A, then B over part of A, then A again). With `knownBlocks` only the cells the swizzle saw change are
    // hashed again (changedBlocks_, cleared here); the others hold their bytes and so their identity.
    void SetUploadIds(const BufferView &view, const PixelRect &rect, bool knownBlocks) {
        const std::uint8_t *memory = memory_.Local().bytes().data();
        const memory::PsmInfo &info = memory::psmInfo(view.psm);
        const std::int32_t bw = info.blockWidth, bh = info.blockHeight;
        for (std::int32_t by = rect.y0 / bh; by <= rect.y1 / bh; ++by) {
            for (std::int32_t bx = rect.x0 / bw; bx <= rect.x1 / bw; ++bx) {
                const std::uint32_t b = BlockOfPixel(view, static_cast<std::uint32_t>(bx * bw),
                                                     static_cast<std::uint32_t>(by * bh));  // IN-456
                const std::uint64_t bit = std::uint64_t{1} << (b & 63u);
                if (!knownBlocks || (changedBlocks_[b >> 6] & bit) != 0) {
                    blockIds_[b] = HashBlock(memory + static_cast<std::size_t>(b) * memory::kBlockBytes);
                }
                changedBlocks_[b >> 6] &= ~bit;
            }
        }
    }

    // The writer of the newest page write inside `span`.
    WriteKind NewestWriter(const BlockSpan &span) const {
        if (span.count == 0 || span.count >= kAllBlocks) return kWriteWhole;
        const std::uint32_t firstPage = span.begin / memory::kBlocksPerPage;
        const std::uint32_t lastBlock = span.begin + span.count - 1u;
        std::uint64_t newest = 0;
        WriteKind kind = kWriteWhole;
        for (std::uint32_t page = firstPage; page <= lastBlock / memory::kBlocksPerPage; ++page) {
            const std::uint32_t p = page % memory::kPagesTotal;
            if (pageWrites_[p] >= newest) {
                newest = pageWrites_[p];
                kind = static_cast<WriteKind>(pageWriter_[p]);
            }
        }
        return kind;
    }

    bool UnchangedSince(const BlockSpan &span, std::uint64_t sequence) const {
        if (span.count >= kAllBlocks) {
            for (std::uint64_t value : pageWrites_) {
                if (value > sequence) return false;
            }
            return true;
        }
        const std::uint32_t firstPage = span.begin / memory::kBlocksPerPage;
        const std::uint32_t lastBlock = span.begin + span.count - 1u;
        for (std::uint32_t page = firstPage; page <= lastBlock / memory::kBlocksPerPage; ++page) {
            if (pageWrites_[page % memory::kPagesTotal] > sequence) return false;
        }
        return true;
    }

    static std::uint64_t SizeKey(std::uint32_t width, std::uint32_t height) {
        return (static_cast<std::uint64_t>(width) << 32) | height;
    }

    // Images of forgotten textures are reused for new ones of the same size (no allocation per decode). A later
    // decode into one is recorded after every draw that sampled it, so it never changes what those draws see.
    ImageId TextureImage(std::uint32_t width, std::uint32_t height) {
        auto &pool = texturePool_[SizeKey(width, height)];
        if (!pool.empty()) {
            const ImageId image = pool.back();
            pool.pop_back();
            return image;
        }
        std::string error;
        const ImageId image = gpu_.CreateImage(width, height, false, error);
        if (image == kNoImage) Fail(error);
        return image;
    }

    void ForgetTexture(std::unordered_map<std::uint64_t, TextureEntry>::iterator it) {
        texturePool_[SizeKey(it->second.key.width, it->second.key.height)].push_back(it->second.image);
        textureBytes_ -= std::min(textureBytes_, it->second.bytes);
        textures_.erase(it);
    }

    // TextureCacheView: a hit returns an empty vector (the texels are already on the GPU, textureHit_ says where).
    const std::vector<std::uint32_t> *find(const writeback::TextureKey &key, const BufferView &) override {
        static const std::vector<std::uint32_t> kOnGpu;
        auto it = textures_.find(KeyHash(key));
        if (it == textures_.end() || !(it->second.key == key)) return nullptr;
        if (!UnchangedSince(it->second.span, it->second.sequence)) {
            ForgetTexture(it);
            return nullptr;
        }
        it->second.lastUse = ++textureUses_;
        textureHit_ = it->second.image;
        return &kOnGpu;
    }

    // A fresh decode: placed in the atlas when the batch is drawn (PlaceInAtlas), keyed here.
    writeback::TextureKey pendingKey_;
    BufferView pendingView_{0, 0, Psm::Ct32};
    bool pendingStore_ = false;
    void store(const writeback::TextureKey &key, const BufferView &view, const std::vector<std::uint32_t> &) override {
        pendingKey_ = key;
        pendingView_ = view;
        pendingStore_ = true;
    }

    // IN-408: the GLM over `span` also holds what drawn targets hold there, written on the GPU (no readback): a GLM
    // reader then sees what a readback would have stored. 32-bit colour targets are written this way, and 32-bit
    // depth targets too (IN-417); any other drawn target over the span is read back as before. Returns whether a
    // target was written (`any`).
    // IN-510: at a race start GT4 loads 8 palettes from one small colour and depth pair over 1,000 times per VBlank, and each
    // load wrote both targets into the GLM again (about 2,240 writes, about 20 ms of CPU and GPU, with nothing drawn in
    // between): the drop the owner saw while the cars roll to the line. Textures over targets do the same with other rows.
    // A target's page row is left out when it has not changed since the GLM got it, and nothing has made the GLM wrong for
    // it since: an upload of those pages from memory, or an older target written over them (newest draw wins, so a newer
    // target written after it does no harm). GT4_CORE2_GLM_MEMO=0 always writes (A/B).
    const bool glmMemo_ = std::getenv("GT4_CORE2_GLM_MEMO") == nullptr || std::strcmp(std::getenv("GT4_CORE2_GLM_MEMO"), "0") != 0;
    std::uint64_t glmWriteSequence_ = 0;
    std::array<std::uint64_t, memory::kPagesTotal> glmPageWrong_{};    // the last event that made targets written before it wrong
    std::array<std::uint64_t, memory::kPagesTotal> glmPageNewest_{};   // the newest draw written since the last upload
    template <typename Visit>
    static void ForEachPageOfSpan(const BlockSpan &span, Visit visit) {
        if (span.count == 0) return;
        if (span.count >= kAllBlocks) {
            for (std::uint32_t page = 0; page < memory::kPagesTotal; ++page) visit(page);
            return;
        }
        const std::uint32_t firstPage = span.begin / memory::kBlocksPerPage;
        const std::uint32_t lastPage = (span.begin + span.count - 1u) / memory::kBlocksPerPage;
        for (std::uint32_t page = firstPage; page <= lastPage; ++page) visit(page % memory::kPagesTotal);
    }
    bool GlmRowHeld(const Target &t, std::uint32_t row, std::uint32_t pageHeight) const {
        if (!glmMemo_ || row >= t.glmRowWritten.size() || t.glmRowWritten[row] == 0 || t.glmHeldDraw != t.lastDraw ||
            t.glmHeldChanges != t.imageChanges) {
            return false;
        }
        const BlockSpan rowSpan = SpanOfRect(t.view, PixelRect{t.drawn.x0, static_cast<std::int32_t>(row * pageHeight), t.drawn.x1,
                                                               static_cast<std::int32_t>((row + 1u) * pageHeight) - 1});
        bool held = true;
        ForEachPageOfSpan(rowSpan, [&](std::uint32_t page) { held = held && glmPageWrong_[page] <= t.glmRowWritten[row]; });
        return held;
    }

    bool MaterializeTargets(const BlockSpan &span, bool &any) {
        any = false;
        for (const Target &t : targets_) {
            if (t.alive && !t.drawn.Empty() && !MaterializableClass(t.cls) && SpansOverlap(TargetSpan(t, t.drawn), span)) {
                return Readback(span);
            }
        }
        std::vector<Target *> order;
        for (Target &t : targets_) {
            if (t.alive && !t.drawn.Empty() && SpansOverlap(TargetSpan(t, t.drawn), span)) order.push_back(&t);
        }
        std::sort(order.begin(), order.end(), [](const Target *a, const Target *b) { return a->lastDraw < b->lastDraw; });
        for (Target *target : order) {
            Target &t = *target;
            if (!ApplyDirty(t)) return false;
            // The target's page rows inside the span, within its drawn area.
            const memory::PsmInfo &info = memory::psmInfo(t.cls == FormatClass::Colour16 ? t.view.psm : Psm::Ct32);
            const std::uint32_t perRow = std::max<std::uint32_t>(t.view.bufferWidth, 1u);
            PixelRect rows;
            bool anyRow = false;
            if (t.glmHeldDraw != t.lastDraw || t.glmHeldChanges != t.imageChanges) {
                t.glmRowWritten.fill(0);
                t.glmHeldDraw = t.lastDraw;
                t.glmHeldChanges = t.imageChanges;
            }
            for (std::uint32_t row = 0; row * info.pageHeight < t.height; ++row) {
                const BlockSpan rowSpan{(t.view.basePointer + row * perRow * memory::kBlocksPerPage) % kAllBlocks,
                                        perRow * memory::kBlocksPerPage};
                if (!SpansOverlap(rowSpan, span)) continue;
                if (Intersect(PixelRect{0, static_cast<std::int32_t>(row * info.pageHeight), static_cast<std::int32_t>(t.width) - 1,
                                        static_cast<std::int32_t>((row + 1) * info.pageHeight) - 1}, t.drawn).Empty()) {
                    continue;
                }
                anyRow = true;
                if (GlmRowHeld(t, row, info.pageHeight)) {
                    stats.materializationsLeftOut++;
                    continue;
                }
                rows = Union(rows, PixelRect{0, static_cast<std::int32_t>(row * info.pageHeight),
                                             static_cast<std::int32_t>(t.width) - 1,
                                             static_cast<std::int32_t>((row + 1) * info.pageHeight) - 1});
            }
            if (anyRow) {
                t.readOnGpu = true;
                any = true;
            }
            const PixelRect area = Intersect(rows, t.drawn);
            if (area.Empty()) continue;
            std::string error;
            const std::uint32_t layout = t.cls == FormatClass::Depth32    ? static_cast<std::uint32_t>(Psm::Z32)
                                         : t.cls == FormatClass::Colour16 ? static_cast<std::uint32_t>(t.view.psm)
                                                                          : static_cast<std::uint32_t>(Psm::Ct32);
            if (!gpu_.MaterializeTarget(t.image, layout, t.view.basePointer, t.view.bufferWidth, area.x0, area.y0,
                                        area.x1, area.y1, t.fullWordWritten, error)) {
                Fail(error);
                return false;
            }
            ++glmWriteSequence_;
            ForEachPageOfSpan(SpanOfRect(t.view, area), [&](std::uint32_t page) {
                if (t.lastDraw < glmPageNewest_[page]) {
                    glmPageWrong_[page] = glmWriteSequence_;  // older pixels over newer ones
                } else {
                    glmPageNewest_[page] = t.lastDraw;
                }
            });
            for (std::uint32_t row = static_cast<std::uint32_t>(area.y0) / info.pageHeight;
                 row <= static_cast<std::uint32_t>(area.y1) / info.pageHeight && row < t.glmRowWritten.size(); ++row) {
                t.glmRowWritten[row] = glmWriteSequence_;
            }
            // Not a use for 6.2 ageing: a readback was not one either, and a target kept alive by reads of its memory
            // later wins texture lookups over newer memory (GT4's title video after GT Mode).
            t.readOnGpu = true;
            any = true;
            stats.materializations++;
            static const bool trace = std::getenv("GT4_CORE2_READBACK_TRACE") != nullptr;  // diagnostics only
            if (trace) {
                std::fprintf(stderr, "[core2] glm write vblank=%llu target bp=%u bw=%u size=%ux%u area=%d,%d,%d,%d span=%u+%u\n",
                             static_cast<unsigned long long>(vblanks_), t.view.basePointer, t.view.bufferWidth, t.width,
                             t.height, area.x0, area.y0, area.x1, area.y1, span.begin, span.count);
            }
        }
        return true;
    }

    // 3.7: the GLM pages a texture reads are made current (uploaded where GS memory changed since).
    bool MakeGlmCurrent(const BlockSpan &span) {
        ScopedUs timer(clock_, &PhaseTimes::glm);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
        static const bool full = std::getenv("GT4_CORE2_GLM_FULL") != nullptr;  // diagnostics A/B
        auto add = [&](std::uint32_t page) {
            if (!full && glmUploaded_[page] && glmSequence_[page] >= pageWrites_[page]) return;
            glmUploaded_[page] = true;
            glmSequence_[page] = writeSequence_;
            times_.glmPages += 1;
            const std::uint32_t offset = page * memory::kPageBytes;
            if (!ranges.empty() && ranges.back().first + ranges.back().second == offset) {
                ranges.back().second += memory::kPageBytes;
            } else {
                ranges.push_back({offset, memory::kPageBytes});
            }
        };
        if (span.count >= kAllBlocks) {
            for (std::uint32_t page = 0; page < memory::kPagesTotal; ++page) add(page);
        } else if (span.count != 0) {
            const std::uint32_t firstPage = span.begin / memory::kBlocksPerPage;
            const std::uint32_t lastBlock = span.begin + span.count - 1u;
            for (std::uint32_t page = firstPage; page <= lastBlock / memory::kBlocksPerPage; ++page) {
                add(page % memory::kPagesTotal);
            }
        }
        std::string error;
        if (!ranges.empty()) {
            times_.glmCalls += 1;
            ++glmWriteSequence_;
            for (const auto &range : ranges) {
                for (std::uint32_t offset = 0; offset < range.second; offset += memory::kPageBytes) {
                    const std::uint32_t page = (range.first + offset) / memory::kPageBytes;
                    glmPageWrong_[page] = glmWriteSequence_;
                    glmPageNewest_[page] = 0;
                }
            }
        }
        if (!gpu_.UploadGlm(memory_.Local().bytes().data(), ranges, error)) {
            Fail(error);
            return false;
        }
        return true;
    }

    // A texture from GS memory (3.7): decoded on the GPU into a 1x image, kept while its pages are unchanged; the
    // least recently used textures go when the cache is over its budget.
    // IN-428: the smallest circular block range holding both spans.
    static BlockSpan UnionSpans(const BlockSpan &a, const BlockSpan &b) {
        if (a.count >= kAllBlocks || b.count >= kAllBlocks) return BlockSpan{0, kAllBlocks};
        if (a.count == 0) return b;
        if (b.count == 0) return a;
        const std::uint64_t fromA = std::max<std::uint64_t>(a.count, ((b.begin + kAllBlocks - a.begin) % kAllBlocks) + std::uint64_t{b.count});
        const std::uint64_t fromB = std::max<std::uint64_t>(b.count, ((a.begin + kAllBlocks - b.begin) % kAllBlocks) + std::uint64_t{a.count});
        if (fromA <= fromB) return BlockSpan{a.begin, static_cast<std::uint32_t>(std::min<std::uint64_t>(fromA, kAllBlocks))};
        return BlockSpan{b.begin, static_cast<std::uint32_t>(std::min<std::uint64_t>(fromB, kAllBlocks))};
    }

    // IN-428 (C1, spec 3.6 "full" mode with base level 0): a mip draw's levels 1..MXL, decoded right of level 0 in the
    // same image (level n >= 1 at x = 2^TW, y = 2^TH - (2^TH >> (n - 1))), and the parameters the pixel shader needs.
    // Only for memory textures without region wrap (their extent is 2^TW x 2^TH); the rest keep level 0.
    // GT4_CORE2_MIPMAP=0 samples level 0 only (A/B).
    const bool mipmap_ = std::getenv("GT4_CORE2_MIPMAP") == nullptr || std::strcmp(std::getenv("GT4_CORE2_MIPMAP"), "0") != 0;
    struct MipPlan {
        std::uint32_t levels = 0;  // 0: level 0 only
        std::uint32_t info = 0;    // the shader's packed parameters
        std::uint32_t imageWidth = 0, imageHeight = 0;
        std::uint64_t hash = 0;
        std::array<lod::MipLevelAddress, 7> address{};
        std::array<lod::MipSize, 7> size{};
    };
    // IN-453: the block key of a texture: its address key hash and the versions of every block its levels read (the
    // block list is computed once per address key).
    std::uint64_t ContentKey(std::uint64_t addressHash, const writeback::BatchTranslation &t, Psm psm, const MipPlan &mip) {
        auto [it, inserted] = blockLists_.try_emplace(addressHash);
        std::vector<std::uint16_t> &blocks = it->second;
        if (inserted) {
            const memory::PsmInfo &info = memory::psmInfo(psm);
            auto addLevel = [&](std::uint32_t bp, std::uint32_t bw, std::uint32_t width, std::uint32_t height) {
                const BufferView view{bp, bw, psm};
                for (std::uint32_t by = 0; by * info.blockHeight < height; ++by) {
                    for (std::uint32_t bx = 0; bx * info.blockWidth < width; ++bx) {
                        const std::uint32_t block =
                            BlockOfPixel(view, (bx * info.blockWidth) % 2048u, (by * info.blockHeight) % 2048u);
                        blocks.push_back(static_cast<std::uint16_t>(block));
                    }
                }
            };
            addLevel(t.glmBp, t.glmBw, t.texWidth, t.texHeight);
            for (std::uint32_t n = 1; n <= mip.levels; ++n) {
                addLevel(mip.address[n].tbp, mip.address[n].tbw, mip.size[n].width, mip.size[n].height);
            }
        }
        std::uint64_t h = MixWords(addressHash, blocks.size());
        for (std::uint16_t block : blocks) h = MixWords(h, blockIds_[block]);
        return h == 0 ? 1u : h;
    }

    MipPlan PlanMips(const writeback::BatchTranslation &t, std::uint64_t tex1, std::uint64_t miptbp1,
                     std::uint64_t miptbp2) const {
        MipPlan plan;
        if (!mipmap_) return plan;
        const lod::Tex1Fields f = lod::decode_tex1(tex1);
        if (f.mxl == 0 || f.mmin < 2 || f.mmin > 5) return plan;
        const std::uint32_t tw = t.state.fields[gpu::gpufield::TexLog2W];
        const std::uint32_t th = t.state.fields[gpu::gpufield::TexLog2H];
        if (tw > 10 || th > 10 || t.texWidth != (1u << tw) || t.texHeight != (1u << th)) return plan;
        const std::uint32_t levels = std::min<std::uint32_t>(f.mxl, 6u);
        const bool packed = lod::mtba_applies(f.mtba, tw, t.glmPsm);
        const std::array<lod::MipLevelAddress, 3> packedLevels =
            packed ? lod::mtba_levels(t.glmBp, tw, t.glmPsm) : std::array<lod::MipLevelAddress, 3>{};
        std::uint64_t hash = 0x6D69705F6C76ull ^ levels;
        for (std::uint32_t n = 1; n <= levels; ++n) {
            plan.address[n] = packed && n <= 3 ? packedLevels[n - 1] : lod::mip_level_address(n, t.glmBp, t.glmBw, miptbp1, miptbp2);
            plan.size[n] = lod::mip_level_size(tw, th, n);
            hash = (hash ^ (static_cast<std::uint64_t>(plan.address[n].tbp) << 8 | plan.address[n].tbw)) * 1099511628211ull;
        }
        plan.levels = levels;
        plan.imageWidth = (1u << tw) + std::max(1u, (1u << tw) >> 1);
        plan.imageHeight = 1u << th;
        plan.hash = hash | 1u;
        plan.info = levels | (f.mmin << 3) | ((f.lcm ? 1u : 0u) << 6) | ((f.l & 3u) << 7) |
                    ((static_cast<std::uint32_t>(f.k_raw) & 0xFFFu) << 9);
        return plan;
    }

    ImageId MemoryTexture(const writeback::BatchTranslation &t, std::uint64_t tex0, std::uint64_t texa, std::uint64_t tex1,
                          std::uint64_t miptbp1, std::uint64_t miptbp2, std::uint32_t &mipInfo) {
        ScopedUs timer(clock_, &PhaseTimes::texture);
        writeback::TextureKey key;
        key.tbp = t.glmBp;
        key.tbw = t.glmBw;
        key.psm = t.glmPsm;
        key.width = t.texWidth;
        key.height = t.texHeight;
        // IN-408: a palette with GPU-written halfwords is expanded on the GPU and keyed by the loads that wrote them.
        const std::uint64_t gpuPalette =
            t.glmIndexed ? GpuPaletteKey(t.glmPsm, tex0::Cpsm(tex0), tex0::Csa(tex0)) : 0;
        if (t.glmIndexed && paletteRows_) {
            key.paletteHash = 0;  // IN-409: the decode holds CLUT indices, whatever the palette
        } else if (t.glmIndexed) {
            key.paletteHash = gpuPalette != 0 ? gpuPalette : writeback::HashPaletteWords(t.palette);
            if (gpuPalette != 0) {
                key.ta0 = static_cast<std::uint32_t>(texa & 0xFFu);
                key.ta1 = static_cast<std::uint32_t>((texa >> 32) & 0xFFu);
                key.aem = static_cast<std::uint32_t>((texa >> 15) & 1u);
            }
        }
        const MipPlan mip = PlanMips(t, tex1, miptbp1, miptbp2);
        mipInfo = mip.info | (anisoLog2_ << 21);  // IN-434
        if (mip.levels > 0) {
            key.width = mip.imageWidth;
            key.height = mip.imageHeight;
            key.mipHash = mip.hash;
            stats.mipTextures++;
        }
        const std::uint64_t hash = KeyHash(key);
        const auto texturePsm = memory::decodePsm(t.glmPsm);
        if (!texturePsm) return kNoImage;
        BlockSpan textureSpan =
            SpanOfRect(BufferView{t.glmBp, t.glmBw, *texturePsm}, 0, 0, static_cast<std::int32_t>(t.texWidth) - 1,
                       static_cast<std::int32_t>(t.texHeight) - 1);
        for (std::uint32_t n = 1; n <= mip.levels; ++n) {  // IN-428: every level's blocks
            textureSpan = UnionSpans(textureSpan, SpanOfRect(BufferView{mip.address[n].tbp, mip.address[n].tbw, *texturePsm}, 0,
                                                             0, static_cast<std::int32_t>(mip.size[n].width) - 1,
                                                             static_cast<std::int32_t>(mip.size[n].height) - 1));
        }
        // IN-408: a texture over drawn targets reads them through the GLM, written on the GPU; such a decode is not
        // kept, since the targets change without GS memory page versions changing.
        bool overTargets = false;
        if (gpuTextures_) {
            for (const Target &target : targets_) {
                if (target.alive && !target.drawn.Empty() && SpansOverlap(TargetSpan(target, target.drawn), textureSpan)) {
                    overTargets = true;
                    break;
                }
            }
        }
        // IN-513: inside a VBlank GT4 asks for the same few textures over targets again and again (George V Paris: 353
        // decodes of 43 textures, one of them 148 times, each writing targets into the GLM). A decode is reused in its
        // VBlank while its pages were not written and the drawn targets over them have not changed.
        std::uint64_t overSignature = 0;
        if (overTargets && overMemoOn_) {
            overSignature = OverTargetsSignature(textureSpan);
            const auto again = overMemo_.find(hash);
            if (again != overMemo_.end() && again->second.key == key && again->second.signature == overSignature &&
                UnchangedSince(textureSpan, again->second.sequence)) {
                stats.overTargetReuses++;
                return again->second.image;
            }
        }
        auto found = textures_.find(hash);
        static const bool cacheOn = std::getenv("GT4_CORE2_TEXCACHE") == nullptr ||
                                    std::strcmp(std::getenv("GT4_CORE2_TEXCACHE"), "0") != 0;  // diagnostics A/B
        if (cacheOn && !overTargets && found != textures_.end() && found->second.key == key &&
            UnchangedSince(found->second.span, found->second.sequence)) {
            found->second.lastUse = ++textureUses_;
            stats.atlasHits++;
            return found->second.image;
        }
        // IN-453: the pages changed, but maybe not this texture's blocks (other blocks of its pages were written).
        std::uint64_t content = 0;
        if (cacheOn && contentCacheOn_ && !overTargets) {
            content = ContentKey(hash, t, *texturePsm, mip);
            if (found != textures_.end() && found->second.key == key && found->second.content == content) {
                found->second.span = textureSpan;
                found->second.sequence = writeSequence_;
                found->second.lastUse = ++textureUses_;
                stats.contentHits++;
                return found->second.image;
            }
        }
        // B2: why this decode happens.
        if (overTargets) {
            stats.decodesOverTargets++;
        } else if (found == textures_.end()) {
            stats.decodesFirst++;
        } else if (!(found->second.key == key)) {
            stats.decodesKeyClash++;
        } else {
            stats.decodesStale++;
            stats.staleBy[NewestWriter(found->second.span)]++;
            static const bool trace = std::getenv("GT4_CORE2_DECODE_TRACE") != nullptr;  // diagnostics only
            if (trace) {
                std::fprintf(stderr, "[core2] stale decode vblank=%llu tbp=%u tbw=%u psm=%02x %ux%u span=%u+%u age=%llu\n",
                             static_cast<unsigned long long>(vblanks_), t.glmBp, t.glmBw, t.glmPsm, t.texWidth,
                             t.texHeight, found->second.span.begin, found->second.span.count,
                             static_cast<unsigned long long>(textureUses_ - found->second.lastUse));
            }
        }
        if (found != textures_.end()) ForgetTexture(found);
        const auto psm = memory::decodePsm(t.glmPsm);
        if (!psm) return kNoImage;
        const BlockSpan span = textureSpan;  // level 0, and every mip level (IN-428)
        // IN-422: a texture over no drawn target reads a copy of its pages taken from CPU memory (current for them)
        // instead of the GLM: no GLM upload, and no barrier between that upload and the decode.
        std::uint32_t windowFirst = 0, windowPages = 0;
        const bool window = decodeWindow_ && !overTargets && mip.levels == 0 && span.count < kAllBlocks && span.count != 0 &&
                            [&] {
                                windowFirst = span.begin / memory::kBlocksPerPage;
                                const std::uint32_t lastPage = (span.begin + span.count - 1u) / memory::kBlocksPerPage;
                                windowPages = lastPage - windowFirst + 1u;
                                return windowPages <= kMaxWindowPages;
                            }();
        if (!window && !MakeGlmCurrent(span)) return kNoImage;
        if (overTargets) {
            bool any = false;
            if (!MaterializeTargets(span, any)) return kNoImage;
            stats.texturesOverTargets++;
        }
        const ImageId image = TextureImage(key.width, key.height);
        if (image == kNoImage) return kNoImage;
        if (window) gpu_.SetDecodeWindow(memory_.Local().bytes().data(), windowFirst, windowPages);
        std::string error;
        if (gpuPalette != 0 && !(t.glmIndexed && paletteRows_) && !FlushClutMirror()) return kNoImage;
        // One level (level 0, or a mip level at its place right of level 0, IN-428), decoded the way the texture needs.
        auto decodeLevel = [&](std::uint32_t bp, std::uint32_t bw, std::uint32_t width, std::uint32_t height,
                               std::uint32_t originX, std::uint32_t originY) {
            if (t.glmIndexed && paletteRows_) {
                return gpu_.DecodeTexture(image, bp, bw, t.glmPsm, width, height, nullptr, error, originX, originY);
            }
            if (gpuPalette != 0) {
                GpuPalette palette;
                palette.cpsm = static_cast<std::uint32_t>(tex0::Cpsm(tex0));
                palette.csa = static_cast<std::uint32_t>(tex0::Csa(tex0));
                palette.texa = texa;
                stats.gpuPaletteDecodes++;
                return gpu_.DecodeTextureGpuClut(image, bp, bw, t.glmPsm, width, height, palette, error, originX, originY);
            }
            return gpu_.DecodeTexture(image, bp, bw, t.glmPsm, width, height, t.glmIndexed ? t.palette.data() : nullptr,
                                      error, originX, originY);
        };
        if (!decodeLevel(t.glmBp, t.glmBw, t.texWidth, t.texHeight, 0, 0)) {
            Fail(error);
            return kNoImage;
        }
        for (std::uint32_t n = 1; n <= mip.levels; ++n) {
            const std::uint32_t originY = t.texHeight - (t.texHeight >> (n - 1));
            if (!decodeLevel(mip.address[n].tbp, mip.address[n].tbw, mip.size[n].width, mip.size[n].height, t.texWidth,
                             originY)) {
                Fail(error);
                return kNoImage;
            }
            stats.mipLevelsDecoded++;
        }
        if (overTargets) {
            // IN-408: decoded from targets' pixels written into the GLM, which change without GS memory page versions
            // changing: never kept (a later hit would show the targets as they were, GT4's title backdrop after GT
            // Mode). The image goes back to the pool at the next VBlank, after every draw that sampled it.
            uncachedTextures_.push_back({image, SizeKey(key.width, key.height)});
            if (overMemoOn_) overMemo_[hash] = OverTargetDecode{key, image, overSignature, writeSequence_};
            return image;
        }
        TextureEntry entry;
        entry.key = key;
        entry.span = span;
        entry.sequence = writeSequence_;
        entry.image = image;
        entry.bytes = static_cast<std::uint64_t>(key.width) * key.height * 4u;
        entry.lastUse = ++textureUses_;
        entry.content = content;
        textureBytes_ += entry.bytes;
        textures_[hash] = entry;
        while (textureBytes_ > textureBudget_ && textures_.size() > 1) {
            auto oldest = textures_.end();
            for (auto it = textures_.begin(); it != textures_.end(); ++it) {
                if (it->first == hash) continue;
                if (oldest == textures_.end() || it->second.lastUse < oldest->second.lastUse) oldest = it;
            }
            if (oldest == textures_.end()) break;
            ForgetTexture(oldest);
        }
        stats.textureBytes = textureBytes_;
        return image;
    }

    // IN-422: GT4_CORE2_DECODE_WINDOW=0 decodes every texture from the GLM (A/B).
    const bool decodeWindow_ = std::getenv("GT4_CORE2_DECODE_WINDOW") == nullptr ||
                               std::strcmp(std::getenv("GT4_CORE2_DECODE_WINDOW"), "0") != 0;
    static constexpr std::uint32_t kMaxWindowPages = 256;  // 2 MiB: larger textures read the GLM

    // IN-408: decodes over drawn targets, used for one batch each; returned to the pool every VBlank.
    std::vector<std::pair<ImageId, std::uint64_t>> uncachedTextures_;

    // IN-513: those decodes of this VBlank, for reuse. GT4_CORE2_OVER_TARGET_MEMO=0 decodes every time (A/B).
    struct OverTargetDecode {
        writeback::TextureKey key;
        ImageId image = kNoImage;
        std::uint64_t signature = 0;
        std::uint64_t sequence = 0;
    };
    const bool overMemoOn_ = std::getenv("GT4_CORE2_OVER_TARGET_MEMO") == nullptr ||
                             std::strcmp(std::getenv("GT4_CORE2_OVER_TARGET_MEMO"), "0") != 0;
    std::unordered_map<std::uint64_t, OverTargetDecode> overMemo_;

    // What a GLM read over `span` depends on, besides memory: every drawn target over it, its content and its drawn area.
    std::uint64_t OverTargetsSignature(const BlockSpan &span) const {
        std::uint64_t hash = 14695981039346656037ull;
        auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
        for (const Target &t : targets_) {
            if (!t.alive || t.drawn.Empty() || !SpansOverlap(TargetSpan(t, t.drawn), span)) continue;
            mix(static_cast<std::uint64_t>(t.image));
            mix(t.lastDraw);
            mix(t.imageChanges);
            mix(t.dirty.size());
            mix(static_cast<std::uint32_t>(t.drawn.x0) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(t.drawn.y0)) << 32));
            mix(static_cast<std::uint32_t>(t.drawn.x1) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(t.drawn.y1)) << 32));
        }
        return hash;
    }

    // Images in the pool past the budget are released (the pool only saves allocations).
    void TrimTexturePool() {
        PumpReplacements(replacementSync_);
        for (const auto &[image, size] : uncachedTextures_) texturePool_[size].push_back(image);
        uncachedTextures_.clear();
        overMemo_.clear();
        std::uint64_t pooled = 0;
        for (auto &[size, images] : texturePool_) {
            const std::uint64_t each = (size >> 32) * (size & 0xFFFFFFFFu) * 4u;
            while (!images.empty() && (pooled + each > textureBudget_ / 4)) {
                gpu_.DestroyImage(images.back());
                images.pop_back();
            }
            pooled += each * images.size();
        }
    }

    // ---- Targets ---------------------------------------------------------------------------------------------
    BlockSpan TargetSpan(const Target &t, const PixelRect &r) const {
        if (!t.spanKnown || r.x0 != t.spanRect.x0 || r.y0 != t.spanRect.y0 || r.x1 != t.spanRect.x1 || r.y1 != t.spanRect.y1 ||
            t.view.basePointer != t.spanView.basePointer || t.view.bufferWidth != t.spanView.bufferWidth || t.view.psm != t.spanView.psm) {
            t.spanValue = SpanOfRect(t.view, r);
            t.spanRect = r;
            t.spanView = t.view;
            t.spanKnown = true;
        }
        return t.spanValue;
    }

    // IN-408: a target whose drawn pixels a GPU CLUT load or texture read without a readback goes to memory before it
    // is dropped (a width change, 6.2 ageing): before IN-408 that first read had already stored it there, and later
    // reads of that memory (GT4's title backdrop after a GT Mode visit) must still see it.
    void KeepGpuReadContent(Target &t) {
        if (!t.alive || !t.readOnGpu || t.drawn.Empty()) return;
        static const bool trace = std::getenv("GT4_CORE2_READBACK_TRACE") != nullptr;  // diagnostics only
        if (trace) {
            std::fprintf(stderr, "[core2] keep vblank=%llu target bp=%u bw=%u psm=%u size=%ux%u drawn=%d,%d,%d,%d last_draw_vblank=%llu unused=%u\n",
                         static_cast<unsigned long long>(vblanks_), t.view.basePointer, t.view.bufferWidth,
                         static_cast<unsigned>(t.view.psm), t.width, t.height, t.drawn.x0, t.drawn.y0, t.drawn.x1, t.drawn.y1,
                         static_cast<unsigned long long>(t.lastDrawVblank), t.unusedFrames);
        }
        readbackReason_ = "keep";
        Readback(TargetSpan(t, t.drawn));
        readbackReason_ = "other";
        stats.gpuReadKeeps++;
    }

    void DestroyTarget(Target &t) {
        if (!t.alive) return;
        gpu_.DestroyImage(t.image);
        t = Target{};
    }

    // Makes the image of `t` hold every rectangle the EE wrote into it (4.1, applied lazily at the next use).
    bool ApplyDirty(Target &t) {
        if (t.dirty.empty()) return true;
        ScopedUs timer(clock_, &PhaseTimes::patch);
        std::vector<Dirty> pending;
        pending.swap(t.dirty);
        for (const Dirty &d : pending) {
            const PixelRect r = Intersect(d.rect, PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1,
                                                             static_cast<std::int32_t>(t.height) - 1});
            if (r.Empty()) continue;
            const std::uint32_t w = static_cast<std::uint32_t>(r.x1 - r.x0 + 1);
            const std::uint32_t h = static_cast<std::uint32_t>(r.y1 - r.y0 + 1);
            const BufferView source{t.view.basePointer, t.view.bufferWidth, d.psm};
            // The words are read on the GPU from the GLM, made current for the rectangle first.
            if (!MakeGlmCurrent(SpanOfRect(source, r))) return false;
            std::string error;
            if (!gpu_.PatchFromGlm(t.image, t.view.basePointer, t.view.bufferWidth, static_cast<std::uint32_t>(d.psm),
                                   r.x0, r.y0, r.x1, r.y1, WriteMaskOf(d.psm), error)) {
                Fail(error);
                return false;
            }
            t.valid = Union(t.valid, r);
            t.imageChanges++;
            stats.dirtyPatches++;
            stats.dirtyPatchPixels += static_cast<std::uint64_t>(w) * h;
        }
        return true;
    }

    bool Grow(Target &t, std::uint32_t width, std::uint32_t height) {
        const std::uint32_t maxSide = gpu_.MaxNativeSide();
        width = std::min(width, maxSide);
        height = std::min(height, maxSide);
        if (width <= t.width && height <= t.height) return true;
        // Rows grow in whole page rows of the target's format, so a target drawn a few rows further each time grows
        // rarely; never more, or its rows past the buffer would overlap the memory after it (GT4's 224-row menu
        // fields sit right before texture memory: 64-row steps made every texture upload patch the display buffer).
        const std::uint32_t pageRows = memory::psmInfo(t.view.psm).pageHeight;
        const std::uint32_t newWidth = std::max(t.width, width);
        const std::uint32_t newHeight =
            std::min(maxSide, std::max(t.height, (height + pageRows - 1u) / pageRows * pageRows));
        std::string error;
        if (!gpu_.GrowImage(t.image, newWidth, newHeight, error)) {
            Fail(error);
            return false;
        }
        t.width = newWidth;
        t.height = newHeight;
        stats.targetsGrown++;
        return true;
    }

    // 2.4b: the areas of a new target that recent EE uploads cover become dirty rectangles.
    void SeedFromRecentUploads(Target &t) {
        const BlockSpan whole = TargetSpan(t, PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1,
                                                        static_cast<std::int32_t>(t.height) - 1});
        for (const RecentUpload &u : recentUploads_) {
            if (drawCounter_ - u.drawNumber > kRecentUploadDraws) continue;
            if (!SpansOverlap(u.span, whole)) continue;
            AddUploadDirty(t, u.view, u.rect, u.span);
        }
    }

    Target *CreateTarget(const BufferView &view, FormatClass cls, const PixelRect &rect) {
        const std::uint32_t maxSide = gpu_.MaxNativeSide();
        const std::uint32_t width = std::min(std::max<std::uint32_t>(view.bufferWidth, 1u) * 64u, maxSide);
        const std::uint32_t pageRows = memory::psmInfo(view.psm).pageHeight;  // see Grow
        const std::uint32_t height = std::min(
            maxSide, (static_cast<std::uint32_t>(std::max(rect.y1, 0)) + pageRows) / pageRows * pageRows);
        std::string error;
        // IN-497: Performance Mode: the small effect targets stay at 1x in a scaled picture, like a lower shadow or
        // reflection resolution in a modern game (reflections, glow reductions; the picture's own target is wider).
        const ImageId image = gpu_.CreateImage(width, height, !(effectTargets1x_ && width <= 256u), error);
        if (image == kNoImage) {
            Fail(error);
            return nullptr;
        }
        Target fresh;
        fresh.alive = true;
        fresh.cls = cls;
        fresh.view = view;
        fresh.image = image;
        fresh.width = width;
        fresh.height = height;
        Target *slot = nullptr;
        for (Target &t : targets_) {
            if (!t.alive) {
                slot = &t;
                break;
            }
        }
        if (!slot) {
            targets_.push_back(Target{});
            slot = &targets_.back();
        }
        *slot = fresh;
        stats.targetsCreated++;
        SeedFromRecentUploads(*slot);
        return slot;
    }

    // IN-509: on George V Paris GT4 draws a 256 x 256 Z target at BP 0 every second VBlank, after drawing the same memory
    // at other widths. One VBlank of grace made it old each time, so it was dropped and its memory kept: six readbacks,
    // four of 640 x 448 targets, every second VBlank, a frametime that swings like a wave. Two VBlanks of grace keep it
    // beside the others. GT4_CORE2_WIDTH_GRACE_LONG=0 gives one VBlank (A/B).
    const std::uint64_t widthGraceVblanks_ = std::getenv("GT4_CORE2_WIDTH_GRACE_LONG") != nullptr &&
                                                     std::strcmp(std::getenv("GT4_CORE2_WIDTH_GRACE_LONG"), "0") == 0
                                                 ? 1u
                                                 : 2u;

    // 2.1, 2.2, 2.4: the target a draw writes (`view` from FRAME or ZBUF).
    Target *DrawTarget(const BufferView &view, const PixelRect &rect) {
        const FormatClass cls = ClassOf(view.psm);
        if (cls == FormatClass::None) return nullptr;
        // 2.1: the target of this base, class and width. One of another width is discarded when it is old (not drawn
        // in this frame or the last two, IN-509); a recent one stays beside the new target (GT4 draws its frame memory
        // as 640 and as 128 pixels wide in the same frame).
        Target *found = nullptr;
        for (Target &t : targets_) {
            if (!t.alive || t.cls != cls || t.view.basePointer != view.basePointer) continue;
            if (t.view.bufferWidth != view.bufferWidth) {
                if (vblanks_ > t.lastDrawVblank + widthGraceVblanks_) {
                    KeepGpuReadContent(t);
                    DestroyTarget(t);
                    stats.targetsReplaced++;
                }
                continue;
            }
            if (!found || t.lastDraw > found->lastDraw) found = &t;
        }
        if (!found) found = CreateTarget(view, cls, rect);
        if (!found) return nullptr;
        if (!Grow(*found, static_cast<std::uint32_t>(std::max(rect.x1, 0)) + 1u,
                  static_cast<std::uint32_t>(std::max(rect.y1, 0)) + 1u)) {
            return nullptr;
        }
        found->view.psm = view.psm;
        return found;
    }

    // 3.2: a colour target whose base equals the texture's and whose format class and width match.
    Target *TextureTarget(const BufferView &view, const PixelRect &footprint) {
        const FormatClass cls = ClassOf(view.psm);
        if (cls != FormatClass::Colour32 && cls != FormatClass::Colour16) return nullptr;
        Target *found = nullptr;
        for (Target &t : targets_) {
            if (!t.alive || t.cls != cls || t.view.basePointer != view.basePointer ||
                t.view.bufferWidth != view.bufferWidth) {
                continue;
            }
            if (!found || t.lastDraw > found->lastDraw) found = &t;
        }
        if (!found) return nullptr;
        if (Intersect(Union(found->valid, found->drawn), footprint).Empty() && found->dirty.empty()) return nullptr;
        return found;
    }

    // ---- Coherence (4) ---------------------------------------------------------------------------------------
    // IN-416: whether a write in `view` lands on target `t` page for page with the same in-page pixel positions: the
    // same swizzle family and Z block rule, base pointers a whole number of pages apart (or any number of blocks when
    // the layout is hierarchical, BlockGridOf), and buffer widths that are whole pages. Each page the write touches is then one page of the target, and the written pixels of that page sit
    // at the same in-page positions in the target. GT4_CORE2_PAGE_EXACT_DIRTY=0 keeps whole page rows (A/B).
    const bool pageExactDirty_ = std::getenv("GT4_CORE2_PAGE_EXACT_DIRTY") == nullptr ||
                                 std::strcmp(std::getenv("GT4_CORE2_PAGE_EXACT_DIRTY"), "0") != 0;
    bool PageExact(const Target &t, const BufferView &view, const PixelRect &rect) const {
        if (!pageExactDirty_) return false;
        if (rect.x0 < 0 || rect.y0 < 0 || rect.x1 >= 2048 || rect.y1 >= 2048) return false;
        const memory::PsmInfo &written = memory::psmInfo(view.psm);
        const memory::PsmInfo &target = memory::psmInfo(t.view.psm);
        if (written.family != target.family || written.zBlockXorApplies != target.zBlockXorApplies) return false;
        // Base pointers a whole number of pages apart, or any number of blocks for a hierarchical, non-Z layout.
        if (((view.basePointer + kAllBlocks - t.view.basePointer) % kAllBlocks) % memory::kBlocksPerPage != 0 &&
            (!BlockGridOf(view.psm).hierarchical || !BlockGridOf(t.view.psm).hierarchical)) {
            return false;
        }
        return view.bufferWidth != 0 && t.view.bufferWidth != 0 && (view.bufferWidth * 64u) % written.pageWidth == 0 &&
               (t.view.bufferWidth * 64u) % written.pageWidth == 0;
    }

    // IN-416: the dirty rectangles of a PageExact write, in the target's coordinates: per page the write touches, its
    // written pixels moved to that page's place in the target (pages outside the target are skipped). Neighbours in
    // one page row are merged.
    void AddPageExactDirty(Target &t, const BufferView &view, const PixelRect &rect) {
        const memory::PsmInfo &info = memory::psmInfo(view.psm);
        const std::int32_t pw = info.pageWidth, ph = info.pageHeight;
        const std::uint32_t writtenPerRow = view.bufferWidth * 64u / info.pageWidth;
        const std::uint32_t targetPerRow = t.view.bufferWidth * 64u / info.pageWidth;
        const std::uint32_t blockOffset = (view.basePointer + kAllBlocks - t.view.basePointer) % kAllBlocks;
        if (blockOffset % memory::kBlocksPerPage != 0) {
            AddBlockExactDirty(t, view, rect, blockOffset);
            return;
        }
        const std::uint32_t pageOffset = blockOffset / memory::kBlocksPerPage;
        const std::size_t first = t.dirty.size();
        for (std::int32_t py = rect.y0 / ph; py <= rect.y1 / ph; ++py) {
            for (std::int32_t px = rect.x0 / pw; px <= rect.x1 / pw; ++px) {
                const std::uint32_t page =
                    (pageOffset + static_cast<std::uint32_t>(py) * writtenPerRow + static_cast<std::uint32_t>(px)) %
                    memory::kPagesTotal;
                const std::int32_t ty = static_cast<std::int32_t>(page / targetPerRow);
                const std::int32_t tx = static_cast<std::int32_t>(page % targetPerRow);
                if (ty * ph >= static_cast<std::int32_t>(t.height)) continue;
                const PixelRect inPage = Intersect(rect, PixelRect{px * pw, py * ph, px * pw + pw - 1, py * ph + ph - 1});
                const std::int32_t dx = (tx - px) * pw, dy = (ty - py) * ph;
                const PixelRect moved{inPage.x0 + dx, inPage.y0 + dy, inPage.x1 + dx, inPage.y1 + dy};
                if (t.dirty.size() > first) {
                    PixelRect &last = t.dirty.back().rect;
                    if (last.y0 == moved.y0 && last.y1 == moved.y1 && last.x1 + 1 == moved.x0) {
                        last.x1 = moved.x1;
                        continue;
                    }
                }
                PushDirty(t, Dirty{moved, view.psm});
            }
        }
        stats.pageExactDirties++;
    }

    // IN-416: the same for base pointers that are not a whole number of pages apart, block cell by block cell: each
    // cell the write touches is one memory block, found in the target through the cell of its block number.
    void AddBlockExactDirty(Target &t, const BufferView &view, const PixelRect &rect, std::uint32_t blockOffset) {
        const memory::PsmInfo &info = memory::psmInfo(view.psm);
        const BlockGrid &grid = BlockGridOf(view.psm);
        const std::int32_t pw = info.pageWidth, ph = info.pageHeight, bw = info.blockWidth, bh = info.blockHeight;
        const std::uint32_t writtenPerRow = view.bufferWidth * 64u / info.pageWidth;
        const std::uint32_t targetPerRow = t.view.bufferWidth * 64u / info.pageWidth;
        const std::size_t first = t.dirty.size();
        for (std::int32_t by = rect.y0 / bh; by <= rect.y1 / bh; ++by) {
            for (std::int32_t bx = rect.x0 / bw; bx <= rect.x1 / bw; ++bx) {
                const std::int32_t x = bx * bw, y = by * bh;
                const std::uint32_t page = static_cast<std::uint32_t>(y / ph) * writtenPerRow + static_cast<std::uint32_t>(x / pw);
                const std::uint32_t cell = static_cast<std::uint32_t>((y % ph) / bh) * grid.cellsX + static_cast<std::uint32_t>((x % pw) / bw);
                const std::uint32_t block = (blockOffset + page * memory::kBlocksPerPage + grid.blockOfCell[cell]) % kAllBlocks;
                const std::uint32_t targetPage = block / memory::kBlocksPerPage;
                const std::uint32_t targetCell = grid.cellOfBlock[block % memory::kBlocksPerPage];
                const std::int32_t ty = static_cast<std::int32_t>((targetPage / targetPerRow) * ph + (targetCell / grid.cellsX) * bh);
                const std::int32_t tx = static_cast<std::int32_t>((targetPage % targetPerRow) * pw + (targetCell % grid.cellsX) * bw);
                if (ty >= static_cast<std::int32_t>(t.height)) continue;
                const PixelRect inCell = Intersect(rect, PixelRect{x, y, x + bw - 1, y + bh - 1});
                const PixelRect moved{inCell.x0 + tx - x, inCell.y0 + ty - y, inCell.x1 + tx - x, inCell.y1 + ty - y};
                if (t.dirty.size() > first) {
                    PixelRect &last = t.dirty.back().rect;
                    if (last.y0 == moved.y0 && last.y1 == moved.y1 && last.x1 + 1 == moved.x0) {
                        last.x1 = moved.x1;
                        continue;
                    }
                }
                PushDirty(t, Dirty{moved, view.psm});
            }
        }
        stats.pageExactDirties++;
    }

    // 4.1: an EE write into `rect` of `view`: a dirty rectangle in every target it lands on.
    // IN-430: a dirty rectangle the target's last pending one (same format) already covers adds nothing, and one that
    // covers the last replaces it: 4.1 patches read GS memory when they are applied, so applying the same words twice
    // in a row equals applying them once. GT4 uploads its car textures piece by piece over the same page rows of its
    // 256 x 128 targets (up to 8 times a row in one VBlank at the pre-race camera cut), one patch each before.
    // GT4_CORE2_DIRTY_MERGE=0 keeps every rectangle (A/B).
    const bool dirtyMerge_ = std::getenv("GT4_CORE2_DIRTY_MERGE") == nullptr ||
                             std::strcmp(std::getenv("GT4_CORE2_DIRTY_MERGE"), "0") != 0;
    void PushDirty(Target &t, const Dirty &d) {
        if (dirtyMerge_ && !t.dirty.empty() && t.dirty.back().psm == d.psm) {
            Dirty &last = t.dirty.back();
            if (Contains(last.rect, d.rect)) {
                stats.dirtyMerged++;
                return;
            }
            if (Contains(d.rect, last.rect)) {
                last.rect = d.rect;
                stats.dirtyMerged++;
                return;
            }
        }
        t.dirty.push_back(d);
    }

    // IN-508 (4.1 says "the overlapped pages"): whether a write shares no page with a target's drawn rectangle, so it
    // changes none of its drawn pixels. SpanOfRect counts whole page rows, so a target drawn narrower than its buffer
    // looks overlapped by what follows its last row: on George V Paris GT4 streams a CT32 texture from page 382 about
    // once per VBlank, the page right after the 192 pixels of the last row its Z32 target at page 351 (BW 4) draws.
    // Smaller textures land in that page too, from block 12225 on. Both the store (StoreDrawnBeforeWrite) and the patch
    // of those drawn rows (AddTouchedPagesDirty) go together: without the store memory is older than the drawn pixels,
    // so a patch over them would bring old pixels back. The target's base must be page aligned for its pages to be the
    // same rectangles in memory. GT4_CORE2_DRAWN_PAGE_TEST=0 stores and patches whole page rows (A/B).
    const bool drawnPageTest_ = std::getenv("GT4_CORE2_DRAWN_PAGE_TEST") == nullptr ||
                                std::strcmp(std::getenv("GT4_CORE2_DRAWN_PAGE_TEST"), "0") != 0;
    bool WriteMissesDrawnPages(const Target &t, const BufferView &view, const PixelRect &rect) const {
        if (!drawnPageTest_ || t.drawn.Empty() || t.view.basePointer % memory::kBlocksPerPage != 0) return false;
        PageSet written, drawn;
        if (!PagesOfRect(view, rect.x0, rect.y0, rect.x1, rect.y1, written) ||
            !PagesOfRect(t.view, t.drawn.x0, t.drawn.y0, t.drawn.x1, t.drawn.y1, drawn)) {
            return false;
        }
        return (written & drawn).none();
    }

    // IN-508: the dirty rectangles of a write that misses the drawn pages: the target's pages the write touches, whole
    // and in the target's format, as 4.1 asks for another layout. They hold no drawn pixel, so memory is the newest
    // content for all of them. Neighbours in one page row are merged.
    void AddTouchedPagesDirty(Target &t, const BufferView &view, const PixelRect &rect) {
        PageSet written;
        if (!PagesOfRect(view, rect.x0, rect.y0, rect.x1, rect.y1, written)) return;
        const memory::PsmInfo &info = memory::psmInfo(t.view.psm);
        const std::int32_t pw = info.pageWidth, ph = info.pageHeight;
        const std::size_t first = t.dirty.size();
        for (std::int32_t y = 0; y < static_cast<std::int32_t>(t.height); y += ph) {
            for (std::int32_t x = 0; x < static_cast<std::int32_t>(t.width); x += pw) {
                const std::uint32_t page = BlockOfPixel(t.view, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) /
                                           memory::kBlocksPerPage;
                if (!written[page]) continue;
                if (t.dirty.size() > first) {
                    PixelRect &last = t.dirty.back().rect;
                    if (last.y0 == y && last.x1 + 1 == x) {
                        last.x1 = x + pw - 1;
                        continue;
                    }
                }
                PushDirty(t, Dirty{PixelRect{x, y, x + pw - 1, y + ph - 1}, t.view.psm});
            }
        }
    }

    // IN-512: a small write over drawn 32-bit pages in another format, GT4's 24 x 12 T4 textures every second VBlank in the
    // car showroom. 4.1 stores the targets (a GPU wait of 5 to 7 ms, the wave of that screen) so that every target over
    // those pages can be patched from a memory that holds them all. The same merge can run on the GPU with no wait, for
    // the touched blocks only: the drawn targets go into the GLM oldest draw first (what a readback does in memory), each
    // target over the blocks takes them from there, and the bits the write changed, known on the CPU, go on top. Memory
    // stays older than the targets for the other bits until the drawn areas are read back, which then writes these too.
    // GT4_CORE2_PATCH_BITS=0 stores and patches page rows (A/B).
    const bool patchBits_ = std::getenv("GT4_CORE2_PATCH_BITS") == nullptr || std::strcmp(std::getenv("GT4_CORE2_PATCH_BITS"), "0") != 0;
    static constexpr std::int64_t kMaxBitPatchPixels = 16384;
    static constexpr std::uint32_t kMaxBitPatchBlocks = 256;
    bool bitPatchWrite_ = false;  // set by StoreDrawnBeforeWrite for the write it was asked about
    std::vector<std::pair<std::uint32_t, std::uint32_t>> bitPatchBits_;
    std::vector<std::uint32_t> bitPatchWords_;

    bool WriteCanPatchBits(const BufferView &view, const PixelRect &rect, const BlockSpan &span) const {
        if (!patchBits_ || rect.x0 < 0 || rect.y0 < 0 || rect.x1 >= 2048 || rect.y1 >= 2048) return false;
        if (static_cast<std::int64_t>(rect.x1 - rect.x0 + 1) * (rect.y1 - rect.y0 + 1) > kMaxBitPatchPixels) return false;
        if (span.count == 0 || span.count > kMaxBitPatchBlocks) return false;
        const FormatClass cls = ClassOf(view.psm);
        bool sharesDrawnPage = false;
        for (const Target &t : targets_) {
            if (!t.alive) continue;
            const BlockSpan whole = TargetSpan(t, PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1, static_cast<std::int32_t>(t.height) - 1});
            if (!SpansOverlap(whole, span)) continue;
            if (t.cls != FormatClass::Colour32 || t.view.basePointer % memory::kBlocksPerPage != 0 || t.view.bufferWidth == 0) {
                return false;
            }
            if (view.basePointer == t.view.basePointer && view.bufferWidth == t.view.bufferWidth && cls == t.cls) return false;
            if (PageExact(t, view, rect)) return false;
            if (!t.drawn.Empty() && !WriteMissesDrawnPages(t, view, rect)) sharesDrawnPage = true;
        }
        return sharesDrawnPage;
    }

    // Runs after the write reached CPU memory; see above. Returns false after a failure.
    bool PatchWrittenBits(const BufferView &view, const PixelRect &rect, const BlockSpan &span) {
        static const bool trace = std::getenv("GT4_CORE2_STORE_TRACE") != nullptr;  // diagnostics only
        if (trace) {
            std::fprintf(stderr, "[core2] bit patch vblank=%llu write bp=%u bw=%u psm=%02x rect=%d,%d,%d,%d span=%u+%u\n", static_cast<unsigned long long>(vblanks_),
                         view.basePointer, view.bufferWidth, static_cast<unsigned>(view.psm), rect.x0, rect.y0, rect.x1, rect.y1, span.begin, span.count);
            std::fflush(stderr);
        }
        bitPatchBits_.clear();
        for (std::int32_t y = rect.y0; y <= rect.y1; ++y) {
            for (std::int32_t x = rect.x0; x <= rect.x1; ++x) {
                const memory::PixelSlot slot = memory::pixelSlot(view, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                const std::uint32_t bits = slot.bitWidth >= 32u ? 0xFFFFFFFFu : (1u << slot.bitWidth) - 1u;
                bitPatchBits_.push_back({slot.byteOffset / 4u, bits << ((slot.byteOffset % 4u) * 8u + slot.bitShift)});
            }
        }
        std::sort(bitPatchBits_.begin(), bitPatchBits_.end());
        std::size_t kept = 0;
        for (std::size_t i = 0; i < bitPatchBits_.size(); ++i) {
            if (kept != 0 && bitPatchBits_[kept - 1].first == bitPatchBits_[i].first) {
                bitPatchBits_[kept - 1].second |= bitPatchBits_[i].second;
            } else {
                bitPatchBits_[kept++] = bitPatchBits_[i];
            }
        }
        bitPatchBits_.resize(kept);
        bool any = false;
        if (!MakeGlmCurrent(span) || !MaterializeTargets(span, any)) return false;
        static constexpr std::uint32_t kWordsPerBlock = 64;
        const std::uint8_t *bytes = memory_.Local().bytes().data();
        for (Target &t : targets_) {
            if (!t.alive) continue;
            const BlockSpan whole = TargetSpan(t, PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1, static_cast<std::int32_t>(t.height) - 1});
            if (!SpansOverlap(whole, span)) continue;
            // Each block touched is one 8 x 8 cell of the target, found as in AddBlockExactDirty.
            const BufferView layout{t.view.basePointer, t.view.bufferWidth, Psm::Ct32};
            const BlockGrid &grid = BlockGridOf(Psm::Ct32);
            const std::uint32_t perRow = t.view.bufferWidth;
            const std::uint32_t basePage = t.view.basePointer / memory::kBlocksPerPage;
            const std::uint32_t ownedBits = WriteMaskOf(t.view.psm);
            for (std::size_t at = 0; at < bitPatchBits_.size();) {
                const std::uint32_t block = bitPatchBits_[at].first / kWordsPerBlock;
                std::size_t end = at;
                while (end < bitPatchBits_.size() && bitPatchBits_[end].first / kWordsPerBlock == block) ++end;
                const auto first = bitPatchBits_.begin() + static_cast<std::ptrdiff_t>(at);
                const auto last = bitPatchBits_.begin() + static_cast<std::ptrdiff_t>(end);
                at = end;
                const std::uint32_t page = (block / memory::kBlocksPerPage + memory::kPagesTotal - basePage) % memory::kPagesTotal;
                const std::uint32_t cell = grid.cellOfBlock[block % memory::kBlocksPerPage];
                const std::int32_t cellX = static_cast<std::int32_t>((page % perRow) * 64u + (cell % grid.cellsX) * 8u);
                const std::int32_t cellY = static_cast<std::int32_t>((page / perRow) * 32u + (cell / grid.cellsX) * 8u);
                if (cellX + 8 > static_cast<std::int32_t>(t.width) || cellY + 8 > static_cast<std::int32_t>(t.height)) continue;
                std::string error;
                if (!gpu_.PatchFromGlm(t.image, t.view.basePointer, t.view.bufferWidth, static_cast<std::uint32_t>(t.view.psm), cellX, cellY,
                                       cellX + 7, cellY + 7, ownedBits, error)) {
                    Fail(error);
                    return false;
                }
                bitPatchWords_.assign(2u * kWordsPerBlock, 0u);
                for (std::int32_t y = 0; y < 8; ++y) {
                    for (std::int32_t x = 0; x < 8; ++x) {
                        const std::uint32_t word = memory::pixelSlot(layout, static_cast<std::uint32_t>(cellX + x),
                                                                     static_cast<std::uint32_t>(cellY + y)).byteOffset / 4u;
                        const auto found = std::lower_bound(first, last, std::make_pair(word, std::uint32_t{0}));
                        std::uint32_t value = 0;
                        std::memcpy(&value, bytes + word * 4u, sizeof value);
                        const std::size_t index = 2u * static_cast<std::size_t>(y * 8 + x);
                        bitPatchWords_[index] = value;
                        bitPatchWords_[index + 1u] = found != last && found->first == word ? found->second & ownedBits : 0u;
                    }
                }
                if (!gpu_.Patch(t.image, cellX, cellY, cellX + 7, cellY + 7, bitPatchWords_, 0u, error)) {
                    Fail(error);
                    return false;
                }
                t.valid = Union(t.valid, PixelRect{cellX, cellY, cellX + 7, cellY + 7});
                t.imageChanges++;
                stats.bitPatches++;
            }
        }
        return true;
    }

    // Returns whether the write kept the target's drawn pixels apart (IN-508): such a target is never replaced by it.
    bool AddUploadDirty(Target &t, const BufferView &view, const PixelRect &rect, const BlockSpan &span) {
        const FormatClass cls = ClassOf(view.psm);
        if (view.basePointer == t.view.basePointer && view.bufferWidth == t.view.bufferWidth && cls == t.cls) {
            PushDirty(t, Dirty{rect, view.psm});
            return false;
        }
        // IN-416: page for page when the layouts agree, unless the write replaces the whole target (whole page rows
        // then, so that 4.1 deletes it below).
        const BlockSpan whole =
            TargetSpan(t, PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1, static_cast<std::int32_t>(t.height) - 1});
        if (PageExact(t, view, rect) && !SpanContains(WholePagesWritten(view, rect), whole)) {
            AddPageExactDirty(t, view, rect);
            return false;
        }
        if (WriteMissesDrawnPages(t, view, rect)) {
            AddTouchedPagesDirty(t, view, rect);
            return true;
        }
        // Another base, width or format: the target's page rows the write touches, read in the target's format.
        const memory::PsmInfo &info = memory::psmInfo(t.view.psm);
        const std::uint32_t pagesPerRow = std::max<std::uint32_t>(
            1u, (std::max<std::uint32_t>(t.view.bufferWidth, 1u) * 64u + info.pageWidth - 1u) / info.pageWidth);
        const std::uint32_t rowBlocks = pagesPerRow * memory::kBlocksPerPage;
        PixelRect rows;
        for (std::uint32_t row = 0; row * info.pageHeight < t.height; ++row) {
            const BlockSpan rowSpan{(t.view.basePointer + row * rowBlocks) % kAllBlocks, rowBlocks};
            if (!SpansOverlap(rowSpan, span)) continue;
            rows = Union(rows, PixelRect{0, static_cast<std::int32_t>(row * info.pageHeight),
                                         static_cast<std::int32_t>(t.width) - 1,
                                         static_cast<std::int32_t>((row + 1) * info.pageHeight) - 1});
        }
        if (!rows.Empty()) PushDirty(t, Dirty{rows, t.view.psm});
        return false;
    }

    void OnMemoryWritten(const BufferView &view, const PixelRect &rect, bool remember, WriteKind kind = kWriteUpload,
                         const BlockSpan *knownSpan = nullptr) {
        ScopedUs writtenTimer(clock_, &PhaseTimes::written);
        const BlockSpan span = knownSpan ? *knownSpan : WriteSpan(view, rect);
        MarkWritten(span, kind);
        if (bitPatchWrite_) {  // IN-512
            bitPatchWrite_ = false;
            if (!PatchWrittenBits(view, rect, span)) return;
            if (remember) RememberUpload(view, rect, span);
            return;
        }
        for (Target &t : targets_) {
            if (!t.alive) continue;
            const BlockSpan whole = TargetSpan(t, PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1,
                                                            static_cast<std::int32_t>(t.height) - 1});
            if (!SpansOverlap(whole, span)) continue;
            const bool drawnKeptApart = AddUploadDirty(t, view, rect, span);
            static const long long traceAt = std::getenv("GT4_CORE2_BATCH_TRACE_AT")
                                                 ? std::atoll(std::getenv("GT4_CORE2_BATCH_TRACE_AT"))
                                                 : -1;  // diagnostics only
            if (traceAt >= 0 && static_cast<long long>(vblanks_) == traceAt && !t.dirty.empty()) {
                const PixelRect &d = t.dirty.back().rect;
                std::fprintf(stderr,
                             "[core2] dirty target=%u/%u/%02x from write %u/%u/%02x rect=%d,%d,%d,%d -> %d,%d,%d,%d\n",
                             t.view.basePointer, t.view.bufferWidth, static_cast<unsigned>(t.view.psm),
                             view.basePointer, view.bufferWidth, static_cast<unsigned>(view.psm), rect.x0, rect.y0,
                             rect.x1, rect.y1, d.x0, d.y0, d.x1, d.y1);
            }
            // 4.1: a target the write covers entirely is deleted, drawn or not: memory now holds newer content for
            // all of it (GT4 streams textures over the memory of its small effect targets; a kept target would later
            // write its old pixels back over the new texture).
            if (!drawnKeptApart && !t.dirty.empty() && t.dirty.back().psm == t.view.psm &&
                Contains(t.dirty.back().rect, t.valid) && !t.valid.Empty()) {
                DestroyTarget(t);
                stats.targetsReplaced++;
            }
        }
        if (remember) RememberUpload(view, rect, span);
    }

    // 2.4b: an EE upload, for the targets created over it in the next kRecentUploadDraws draws.
    void RememberUpload(const BufferView &view, const PixelRect &rect, const BlockSpan &span) {
        recentUploads_.push_back(RecentUpload{view, rect, span, drawCounter_});
        while (!recentUploads_.empty() && drawCounter_ - recentUploads_.front().drawNumber > kRecentUploadDraws) {
            recentUploads_.pop_front();
        }
        while (recentUploads_.size() > 4096) recentUploads_.pop_front();
    }

    // 4.2: every target over `span` writes its drawn area (inside `limit` when it is given in the same buffer) back
    // to GS memory, oldest draw first so the newest content wins.
    // IN-429: when no target after the first has pending dirty rectangles (4.1 patches read GS memory, which the
    // earlier targets' readbacks change), all the reductions go in one submission with one wait; the memory writes
    // keep the same order, so the result is the same. GT4's pre-race camera cut downloads small areas that span up to
    // 35 targets in one VBlank, one GPU round trip each before. GT4_CORE2_BATCH_READBACK=0 reduces one by one (A/B).
    const bool batchReadback_ = std::getenv("GT4_CORE2_BATCH_READBACK") == nullptr ||
                                std::strcmp(std::getenv("GT4_CORE2_BATCH_READBACK"), "0") != 0;
    bool Readback(const BlockSpan &span, const BufferView *limitView = nullptr, const PixelRect *limit = nullptr) {
        ScopedUs timer(clock_, &PhaseTimes::readback);
        std::vector<Target *> order;
        for (Target &t : targets_) {
            if (!t.alive || t.drawn.Empty()) continue;
            if (!SpansOverlap(TargetSpan(t, t.drawn), span)) continue;
            order.push_back(&t);
        }
        std::sort(order.begin(), order.end(), [](const Target *a, const Target *b) { return a->lastDraw < b->lastDraw; });
        std::vector<PixelRect> areas;
        std::vector<Target *> reduced;
        bool batch = batchReadback_;
        for (Target *t : order) {
            PixelRect area = t->drawn;
            if (limitView && limit && limitView->basePointer == t->view.basePointer &&
                limitView->bufferWidth == t->view.bufferWidth && ClassOf(limitView->psm) == t->cls) {
                area = Intersect(area, *limit);
            }
            area = Intersect(area, PixelRect{0, 0, static_cast<std::int32_t>(t->width) - 1,
                                             static_cast<std::int32_t>(t->height) - 1});
            if (area.Empty()) continue;
            if (!reduced.empty() && !t->dirty.empty()) batch = false;
            reduced.push_back(t);
            areas.push_back(area);
        }
        std::vector<std::vector<std::uint32_t>> batched;
        if (batch && reduced.size() > 1) {
            if (!ApplyDirty(*reduced.front())) return false;
            std::vector<Gpu::ReduceRequest> requests;
            for (std::size_t i = 0; i < reduced.size(); ++i) {
                requests.push_back(Gpu::ReduceRequest{reduced[i]->image, areas[i].x0, areas[i].y0, areas[i].x1,
                                                      areas[i].y1});
            }
            std::string error;
            const std::uint64_t waitBefore = gpu_.Stats().waitNs;
            if (!gpu_.ReduceMany(requests, batched, error)) {
                if (!error.empty()) {
                    Fail(error);
                    return false;
                }
                batched.clear();  // too large together: one by one below
            } else {
                ReasonWait &reasonWait = WaitOf(readbackReason_);
                reasonWait.waitNs += gpu_.Stats().waitNs - waitBefore;
                stats.batchedReadbacks += reduced.size();
            }
        }
        for (std::size_t i = 0; i < reduced.size(); ++i) {
            Target *t = reduced[i];
            const PixelRect area = areas[i];
            std::vector<std::uint32_t> words;
            if (!batched.empty()) {
                words.swap(batched[i]);
                WaitOf(readbackReason_).readbacks++;
            } else {
                if (!ApplyDirty(*t)) return false;
                std::string error;
                const std::uint64_t waitBefore = gpu_.Stats().waitNs;
                if (!gpu_.Reduce(t->image, area.x0, area.y0, area.x1, area.y1, words, error)) {
                    Fail(error);
                    return false;
                }
                ReasonWait &reasonWait = WaitOf(readbackReason_);
                reasonWait.waitNs += gpu_.Stats().waitNs - waitBefore;
                reasonWait.readbacks++;
            }
            const Psm psm = ReadbackPsm(*t);
            const BufferView out{t->view.basePointer, t->view.bufferWidth, psm};
            static const bool trace = std::getenv("GT4_CORE2_READBACK_TRACE") != nullptr;  // diagnostics only
            if (trace) {
                std::fprintf(stderr, "[core2] readback %s vblank=%llu target bp=%u bw=%u psm=%u size=%ux%u area=%d,%d,%d,%d "
                             "span=%u+%u\n", readbackReason_, static_cast<unsigned long long>(vblanks_), t->view.basePointer,
                             t->view.bufferWidth, static_cast<unsigned>(psm), t->width, t->height, area.x0, area.y0,
                             area.x1, area.y1, span.begin, span.count);
            }
            const std::uint32_t w = static_cast<std::uint32_t>(area.x1 - area.x0 + 1);
            const std::uint32_t h = static_cast<std::uint32_t>(area.y1 - area.y0 + 1);
            memory_.Local().writeRect(out, static_cast<std::uint32_t>(area.x0), static_cast<std::uint32_t>(area.y0), w, h,
                                      words.data());
            MarkWritten(SpanOfRect(out, area), kWriteReadback);
            baseWrites_[BaseWriteKey(out.basePointer, out.bufferWidth, static_cast<std::uint32_t>(out.psm))] = writeSequence_;
            if (Contains(area, t->drawn)) {
                t->drawn = PixelRect{};
                t->readOnGpu = false;
            }
            stats.readbacks++;
            stats.readbackPixels += static_cast<std::uint64_t>(w) * h;
        }
        return true;
    }

    bool ReadbackAll() { return Readback(BlockSpan{0, kAllBlocks}); }

    // The format a target's words are written back to GS memory in: a 32/24 target that was only ever drawn as 24-bit
    // leaves the top byte to memory.
    static Psm ReadbackPsm(const Target &t) {
        if (t.cls == FormatClass::Colour32) return t.fullWordWritten ? Psm::Ct32 : Psm::Ct24;
        if (t.cls == FormatClass::Depth32) return t.fullWordWritten ? Psm::Z32 : Psm::Z24;
        return t.view.psm;
    }

    // IN-439: answers an EE download of `rect` in `view` from the previous same download (see delayed_). Returns
    // false when it did not answer (the caller reads back as before); a failure is recorded with Fail.
    bool AnswerDelayedDownload(const BufferView &view, const PixelRect &rect) {
        const BlockSpan span = SpanOfRect(view, rect);
        static const bool trace = std::getenv("GT4_CORE2_READBACK_TRACE") != nullptr;  // diagnostics only
        auto decline = [&](const char *why) {
            if (trace) {
                std::fprintf(stderr, "[core2] delayed download declined vblank=%llu bp=%u: %s\n",
                             static_cast<unsigned long long>(vblanks_), view.basePointer, why);
            }
            return false;
        };
        Target *only = nullptr;
        for (Target &t : targets_) {
            if (!t.alive || t.drawn.Empty() || !SpansOverlap(TargetSpan(t, t.drawn), span)) continue;
            if (only) return decline("several targets");  // the exact path orders them
            only = &t;
        }
        if (!only) return decline("no target");
        if (!only->dirty.empty()) return decline("dirty rectangles");
        if (view.basePointer != only->view.basePointer || view.bufferWidth != only->view.bufferWidth ||
            ClassOf(view.psm) != only->cls) {
            return decline("another view");
        }
        if (!Contains(PixelRect{0, 0, static_cast<std::int32_t>(only->width) - 1,
                                static_cast<std::int32_t>(only->height) - 1},
                      rect)) {
            return decline("outside the target");
        }
        const std::array<std::uint32_t, 7> key{view.basePointer, view.bufferWidth, static_cast<std::uint32_t>(view.psm),
                                               static_cast<std::uint32_t>(rect.x0), static_cast<std::uint32_t>(rect.y0),
                                               static_cast<std::uint32_t>(rect.x1), static_cast<std::uint32_t>(rect.y1)};
        DelayedDownload &d = delayed_[key];
        std::string error;
        if (d.pending) {  // the previous download's words (GT4 recreates the target between them; the words are kept)
            std::vector<std::uint32_t> words;
            if (gpu_.TakeReduced(d.ticket, words, error)) {
                d.words.swap(words);
                d.pending = false;
            } else if (!error.empty()) {
                Fail(error);
                return false;
            }
        }
        bool answered = false;
        if (!d.words.empty()) {
            const BufferView out{only->view.basePointer, only->view.bufferWidth, ReadbackPsm(*only)};
            memory_.Local().writeRect(out, static_cast<std::uint32_t>(rect.x0), static_cast<std::uint32_t>(rect.y0),
                                      static_cast<std::uint32_t>(rect.x1 - rect.x0 + 1),
                                      static_cast<std::uint32_t>(rect.y1 - rect.y0 + 1), d.words.data());
            MarkWritten(SpanOfRect(out, rect), kWriteDelayed);
            baseWrites_[BaseWriteKey(out.basePointer, out.bufferWidth, static_cast<std::uint32_t>(out.psm))] = writeSequence_;
            stats.delayedDownloads++;
            answered = true;
        }
        if (!d.pending) {
            std::uint32_t ticket = 0;
            if (gpu_.ReduceLater(only->image, rect.x0, rect.y0, rect.x1, rect.y1, ticket, error)) {
                d.pending = true;
                d.ticket = ticket;
            } else if (!error.empty()) {
                Fail(error);
                return false;
            }
        }
        return answered;
    }

    // IN-411: before a write into GS memory (an EE upload, a local move) lands, a drawn target that will get WHOLE page
    // rows as dirty (4.1: a write in another base, width or format, or one past the 2048 range) and whose drawn pixels
    // the GPU path read without a readback (IN-408) stores its drawn area first. Those rows are patched back from
    // memory, so memory must hold the target's newest pixels outside the written words; before IN-408 the readback of
    // that GPU read had stored them. The whole drawn area goes, so the target needs this once per round of reads.
    // A write in the target's own view patches only the written rectangle and needs nothing.
    // GT4_CORE2_STORE_BEFORE_WRITE=0 turns it off (A/B).
    // IN-415: no store when the write replaces every bit of the whole pages under the target's drawn area: the stored
    // pixels would all be overwritten. GT4 uploads its streamed textures (64 x 1440 to 64 x 1728 CT32) over the memory
    // of its night render and palette targets once per VBlank, so each store was a GPU wait for nothing.
    // GT4_CORE2_STORE_SKIP_WHOLE=0 stores anyway (A/B).
    const bool storeSkipWhole_ = std::getenv("GT4_CORE2_STORE_SKIP_WHOLE") == nullptr ||
                                 std::strcmp(std::getenv("GT4_CORE2_STORE_SKIP_WHOLE"), "0") != 0;
    std::uint64_t storesSkipped_ = 0;  // since the last [core2] line
    bool StoreDrawnBeforeWrite(const BufferView &view, const PixelRect &rect, const BlockSpan *knownSpan = nullptr) {
        static const bool enabled = std::getenv("GT4_CORE2_STORE_BEFORE_WRITE") == nullptr ||
                                    std::strcmp(std::getenv("GT4_CORE2_STORE_BEFORE_WRITE"), "0") != 0;
        if (!enabled) return true;
        const bool outOfRange = rect.x1 >= 2048 || rect.y1 >= 2048;
        const BlockSpan span = outOfRange ? BlockSpan{0, kAllBlocks} : knownSpan ? *knownSpan : WriteSpan(view, rect);
        const FormatClass cls = ClassOf(view.psm);
        bitPatchWrite_ = false;
        const std::uint64_t storesBeforeThisWrite = stats.storesBeforeWrite;
        const BlockSpan wholePages = storeSkipWhole_ && !outOfRange ? WholePagesWritten(view, rect) : BlockSpan{};
        for (Target &t : targets_) {
            if (!t.alive || !t.readOnGpu || t.drawn.Empty()) continue;
            const bool sameView =
                view.basePointer == t.view.basePointer && view.bufferWidth == t.view.bufferWidth && cls == t.cls;
            if (sameView && !outOfRange) continue;
            if (!SpansOverlap(TargetSpan(t, t.drawn), span)) continue;
            if (PageExact(t, view, rect)) {  // IN-416: only the written pixels get patched
                storesSkipped_++;
                continue;
            }
            if (wholePages.count != 0 && SpanContains(wholePages, TargetSpan(t, t.drawn))) {
                storesSkipped_++;
                continue;
            }
            if (WriteMissesDrawnPages(t, view, rect)) {  // IN-508: no drawn pixel can be patched
                storesSkipped_++;
                stats.drawnPagesMissed++;
                continue;
            }
            // IN-512: the first target that really needs a store tries the merge on the GPU instead.
            if (!outOfRange && stats.storesBeforeWrite == storesBeforeThisWrite && WriteCanPatchBits(view, rect, span)) {
                // Older dirty rectangles of these targets go in first: their patches must not see this write's memory.
                for (Target &dirtyTarget : targets_) {
                    if (!dirtyTarget.alive || dirtyTarget.dirty.empty()) continue;
                    const BlockSpan whole = TargetSpan(dirtyTarget, PixelRect{0, 0, static_cast<std::int32_t>(dirtyTarget.width) - 1,
                                                                             static_cast<std::int32_t>(dirtyTarget.height) - 1});
                    if (SpansOverlap(whole, span) && !ApplyDirty(dirtyTarget)) return false;
                }
                bitPatchWrite_ = true;
                storesSkipped_++;
                return true;
            }
            static const bool trace = std::getenv("GT4_CORE2_STORE_TRACE") != nullptr;  // diagnostics only
            if (trace) {
                std::fprintf(stderr,
                             "[core2] store vblank=%llu write bp=%u bw=%u psm=%02x rect=%d,%d,%d,%d target bp=%u bw=%u "
                             "size=%ux%u drawn=%d,%d,%d,%d\n",
                             static_cast<unsigned long long>(vblanks_), view.basePointer, view.bufferWidth,
                             static_cast<unsigned>(view.psm), rect.x0, rect.y0, rect.x1, rect.y1, t.view.basePointer,
                             t.view.bufferWidth, t.width, t.height, t.drawn.x0, t.drawn.y0, t.drawn.x1, t.drawn.y1);
            }
            readbackReason_ = "before write";
            const bool ok = Readback(TargetSpan(t, t.drawn));
            readbackReason_ = "other";
            if (!ok) return false;
            stats.storesBeforeWrite++;
        }
        return true;
    }

    // ---- TransferSink ----------------------------------------------------------------------------------------
    void InvalidateDestination(const UploadCommit &c) override { memory_.InvalidateDestination(c); }

    // IN-414: GT4_CORE2_SWIZZLE_RUNS=0 swizzles EE uploads pixel by pixel (A/B).
    const UploadSwizzle uploadSwizzle_ = std::getenv("GT4_CORE2_SWIZZLE_RUNS") != nullptr &&
                                                 std::strcmp(std::getenv("GT4_CORE2_SWIZZLE_RUNS"), "0") == 0
                                             ? UploadSwizzle::PerPixel
                                             : UploadSwizzle::PageRuns;

    // IN-486: GT4 sends about 100 textures every VBlank (2.7 MiB in a race), the same bytes to the same places, but over
    // each other (A, then B over part of A, then A again), so memory changes every time and nothing short of doing the
    // write can be skipped. An upload seen twice keeps the blocks it leaves in memory: the next time, the bytes are
    // recognised by their hash and the blocks are compared and copied instead of swizzled pixel by pixel. Only uploads
    // of whole blocks whose every byte belongs to the format, at a page-aligned base. A collision would write a wrong
    // texture; at 64 bits that is far below every other risk in the renderer (IN-453).
    // GT4_CORE2_UPLOAD_MEMO=0 swizzles every upload (A/B).
    struct UploadMemo {
        std::vector<std::uint16_t> blocks;
        std::vector<std::uint64_t> ids;  // the hash of each block's 256 bytes
        std::vector<std::uint8_t> bytes;
        std::uint64_t lastVblank = 0;
    };
    const bool uploadMemoOn_ = std::getenv("GT4_CORE2_UPLOAD_MEMO") == nullptr ||
                               std::strcmp(std::getenv("GT4_CORE2_UPLOAD_MEMO"), "0") != 0;
    static constexpr std::size_t kUploadMemoBudgetBytes = std::size_t{64} << 20;
    std::unordered_map<std::uint64_t, UploadMemo> uploadMemo_;
    std::size_t uploadMemoBytes_ = 0;
    std::array<std::uint64_t, 4096> uploadsSeenOnce_{};

    static bool UploadWritesWholeBlocks(const UploadCommit &c, const memory::PsmInfo &info, memory::Psm psm) {
        using memory::Psm;
        const bool everyByteBelongs = psm == Psm::Ct32 || psm == Psm::Ct16 || psm == Psm::Ct16S || psm == Psm::T8 ||
                                      psm == Psm::T4 || psm == Psm::Z32 || psm == Psm::Z16 || psm == Psm::Z16S;
        return everyByteBelongs && c.dbp % memory::kBlocksPerPage == 0 && c.dsax % info.blockWidth == 0 &&
               c.dsay % info.blockHeight == 0 && c.rrw % info.blockWidth == 0 && c.rrh % info.blockHeight == 0 &&
               c.dsax + c.rrw <= 2048u && c.dsay + c.rrh <= 2048u;
    }

    // The same result as swizzling the bytes again: `changed` and the identities of the changed blocks included.
    void ReplayUpload(const UploadMemo &memo, bool *changed) {
        std::uint8_t *memory = memory_.Local().bytes().data();
        bool any = false;
        for (std::size_t i = 0; i < memo.blocks.size(); ++i) {
            const std::uint32_t block = memo.blocks[i];
            std::uint8_t *out = memory + static_cast<std::size_t>(block) * memory::kBlockBytes;
            const std::uint8_t *in = memo.bytes.data() + i * memory::kBlockBytes;
            // The block already holds these bytes by its identity (IN-453): nothing is read, in memory or in the memo.
            if (blockIds_[block] == memo.ids[i]) continue;
            if (std::memcmp(out, in, memory::kBlockBytes) != 0) {
                std::memcpy(out, in, memory::kBlockBytes);
                blockIds_[block] = memo.ids[i];
                any = true;
            }
        }
        *changed = any;
    }

    void RememberUploadBytes(std::uint64_t key, const BufferView &view, const PixelRect &rect) {
        const memory::PsmInfo &info = memory::psmInfo(view.psm);
        const std::uint8_t *memory = memory_.Local().bytes().data();
        UploadMemo memo;
        memo.lastVblank = vblanks_;
        const std::size_t cells = static_cast<std::size_t>((rect.x1 - rect.x0 + 1) / info.blockWidth) *
                                  static_cast<std::size_t>((rect.y1 - rect.y0 + 1) / info.blockHeight);
        memo.blocks.reserve(cells);
        memo.ids.reserve(cells);
        memo.bytes.resize(cells * memory::kBlockBytes);
        std::size_t i = 0;
        for (std::int32_t y = rect.y0; y <= rect.y1; y += info.blockHeight) {
            for (std::int32_t x = rect.x0; x <= rect.x1; x += info.blockWidth, ++i) {
                const std::uint32_t block = BlockOfPixel(view, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                const std::uint8_t *from = memory + static_cast<std::size_t>(block) * memory::kBlockBytes;
                memo.blocks.push_back(static_cast<std::uint16_t>(block));
                memo.ids.push_back(HashBlock(from));
                std::memcpy(memo.bytes.data() + i * memory::kBlockBytes, from, memory::kBlockBytes);
            }
        }
        if (uploadMemoBytes_ + memo.bytes.size() > kUploadMemoBudgetBytes) {
            // Over the budget (a new track, say): what was not used in the last seconds goes, or everything.
            for (auto it = uploadMemo_.begin(); it != uploadMemo_.end();) {
                if (vblanks_ - it->second.lastVblank > 300u) {
                    uploadMemoBytes_ -= it->second.bytes.size();
                    it = uploadMemo_.erase(it);
                } else {
                    ++it;
                }
            }
            if (uploadMemoBytes_ + memo.bytes.size() > kUploadMemoBudgetBytes) {
                uploadMemo_.clear();
                uploadMemoBytes_ = 0;
            }
        }
        uploadMemoBytes_ += memo.bytes.size();
        uploadMemo_.emplace(key, std::move(memo));
    }

    // Diagnostics: GT4_CORE2_ID_AUDIT=1 compares every block whose identity is a hash of its bytes with those bytes.
    void AuditIds(const char *where) {
        static const bool on = std::getenv("GT4_CORE2_ID_AUDIT") != nullptr;
        static int reports = 0;
        if (!on || reports >= 40) return;
        const std::uint8_t *local = memory_.Local().bytes().data();
        for (std::uint32_t b = 0; b < kAllBlocks && reports < 40; ++b) {
            if (blockIds_[b] < 0x0001000000000000ull) continue;
            if (HashBlock(local + std::size_t{b} * memory::kBlockBytes) != blockIds_[b]) {
                std::fprintf(stderr, "[core2] id audit %s vblank=%llu block=%u writer=%u\n", where,
                             static_cast<unsigned long long>(vblanks_), b,
                             static_cast<unsigned>(pageWriter_[b / memory::kBlocksPerPage % memory::kPagesTotal]));
                ++reports;
            }
        }
    }

    void CommitUpload(const UploadCommit &c, const std::uint8_t *bytes, std::size_t byteCount) override {
        struct AuditOnExit {
            Impl *self;
            ~AuditOnExit() { self->AuditIds("after upload"); }
        } auditOnExit{this};
        AuditIds("before upload");
        ScopedUs uploadTimer(clock_, &PhaseTimes::upload);
        NoteBaseWrite noteBaseWrite{*this, BaseWriteKey(c.dbp, c.dbw, c.dpsm)};
        times_.uploads += 1;
        const auto psm = memory::decodePsm(c.dpsm);
        BlockSpan writtenSpan{};  // IN-487: one walk over the block cells per upload
        const bool spanKnown = psm && c.rrw != 0 && c.rrh != 0 && c.dsax + c.rrw <= 2048u && c.dsay + c.rrh <= 2048u;
        if (psm && c.rrw != 0 && c.rrh != 0) {
            const PixelRect written{static_cast<std::int32_t>(c.dsax), static_cast<std::int32_t>(c.dsay),
                                    static_cast<std::int32_t>(c.dsax + c.rrw - 1u),
                                    static_cast<std::int32_t>(c.dsay + c.rrh - 1u)};
            if (spanKnown) writtenSpan = WriteSpan(BufferView{c.dbp, c.dbw, *psm}, written);
            if (!StoreDrawnBeforeWrite(BufferView{c.dbp, c.dbw, *psm}, written, spanKnown ? &writtenSpan : nullptr)) return;
        }
        static const bool fast = std::getenv("GT4_CORE2_FAST_UPLOAD") == nullptr ||
                                 std::strcmp(std::getenv("GT4_CORE2_FAST_UPLOAD"), "0") != 0;  // A/B
        bool swizzled = false;
        bool changed = true;
        bool replayed = false;
        std::uint64_t memoKey = 0;
        bool rememberThis = false;
        {
            ScopedUs swizzleTimer(clock_, &PhaseTimes::swizzle);
            times_.swizzleBytes += static_cast<double>(byteCount);
            if (uploadMemoOn_ && fast && psm && contentCacheOn_ && uploadSwizzle_ == UploadSwizzle::PageRuns &&
                c.rrw != 0 && c.rrh != 0 && UploadWritesWholeBlocks(c, memory::psmInfo(*psm), *psm)) {
                const std::uint64_t needed = (static_cast<std::uint64_t>(c.rrw) * c.rrh * memory::psmInfo(*psm).transferBpp + 7u) / 8u;
                if (needed <= byteCount) {
                    std::uint64_t seed = c.dbp | (std::uint64_t{c.dbw} << 14) | (std::uint64_t{c.dpsm} << 20);
                    seed = MixWords(seed, c.dsax | (std::uint64_t{c.dsay} << 16) | (std::uint64_t{c.rrw} << 32) |
                                              (std::uint64_t{c.rrh} << 48));
                    memoKey = HashUploadBytes(bytes, static_cast<std::size_t>(needed), seed);
                    const auto found = uploadMemo_.find(memoKey);
                    if (found != uploadMemo_.end()) {
                        ReplayUpload(found->second, &changed);
                        found->second.lastVblank = vblanks_;
                        swizzled = true;
                        replayed = true;
                        stats.uploadsReplayed++;
                        stats.uploadBytesReplayed += needed;
                    } else {
                        std::uint64_t &seen = uploadsSeenOnce_[memoKey & (uploadsSeenOnce_.size() - 1u)];
                        rememberThis = seen == memoKey;
                        seen = memoKey;
                    }
                }
            }
            if (!replayed) {
                swizzled = fast && psm &&
                           FastHostToLocal(memory_.Local().bytes().data(), BufferView{c.dbp, c.dbw, *psm}, c.dsax, c.dsay,
                                           c.rrw, c.rrh, bytes, byteCount, uploadSwizzle_, &changed,
                                           contentCacheOn_ ? changedBlocks_.data() : nullptr);
                if (!swizzled) memory_.CommitUpload(c, bytes, byteCount);
            }
        }
        if (swizzled) {
            // Diagnostics: GT4_CORE2_UPLOAD_CHECK=1 reads the rectangle back through the generic addressing
            // (memory::pixelSlot) and compares every pixel with the host data.
            static const bool check = std::getenv("GT4_CORE2_UPLOAD_CHECK") != nullptr;
            if (check) {
                const BufferView view{c.dbp, c.dbw, *psm};
                const std::uint32_t bpp = memory::psmInfo(*psm).transferBpp;
                std::uint64_t index = 0;
                bool same = true;
                for (std::uint32_t row = 0; row < c.rrh && same; ++row) {
                    for (std::uint32_t column = 0; column < c.rrw; ++column, ++index) {
                        std::uint32_t expected = 0;  // the host texel (little endian, 4-bit low nibble first)
                        if (bpp == 4) {
                            expected = (index & 1u) ? (bytes[index / 2] >> 4) : (bytes[index / 2] & 0x0Fu);
                        } else {
                            std::memcpy(&expected, bytes + index * (bpp / 8u), bpp / 8u);
                        }
                        const std::uint32_t x = (c.dsax + column) % memory::kTransferCoordinateWrap;
                        const std::uint32_t y = (c.dsay + row) % memory::kTransferCoordinateWrap;
                        if (memory_.Local().readPixel(view, x, y) != expected) {
                            same = false;
                            break;
                        }
                    }
                }
                stats.uploadChecks++;
                if (!same) {
                    stats.uploadCheckMismatches++;
                    std::size_t blocksOff = 0, idStale = 0;
                    std::uint64_t firstBlock = 0;
                    if (replayed) {
                        const UploadMemo &memo = uploadMemo_.at(memoKey);
                        const std::uint8_t *local = memory_.Local().bytes().data();
                        for (std::size_t i = 0; i < memo.blocks.size(); ++i) {
                            const std::uint32_t block = memo.blocks[i];
                            if (std::memcmp(local + std::size_t{block} * memory::kBlockBytes,
                                            memo.bytes.data() + i * memory::kBlockBytes, memory::kBlockBytes) != 0) {
                                if (blocksOff++ == 0) firstBlock = block;
                                if (blockIds_[block] == memo.ids[i]) idStale++;
                            }
                        }
                    }
                    std::fprintf(stderr,
                                 "[core2] upload check mismatch vblank=%llu bp=%u bw=%u psm=%02x rect=%u,%u %ux%u replayed=%d "
                                 "blocks_off=%zu id_stale=%zu first=%llu writer=%u\n",
                                 static_cast<unsigned long long>(vblanks_), c.dbp, c.dbw, c.dpsm, c.dsax, c.dsay, c.rrw, c.rrh,
                                 replayed ? 1 : 0, blocksOff, idStale, static_cast<unsigned long long>(firstBlock),
                                 static_cast<unsigned>(pageWriter_[firstBlock / memory::kBlocksPerPage % memory::kPagesTotal]));
                }
            }
        }
        if (!psm || c.rrw == 0 || c.rrh == 0) {
            MarkWritten(BlockSpan{0, kAllBlocks});
            return;
        }
        const BufferView view{c.dbp, c.dbw, *psm};
        const PixelRect rect{static_cast<std::int32_t>(c.dsax), static_cast<std::int32_t>(c.dsay),
                             static_cast<std::int32_t>(c.dsax + c.rrw - 1u), static_cast<std::int32_t>(c.dsay + c.rrh - 1u)};
        if (rect.x1 >= 2048 || rect.y1 >= 2048) {
            std::fill(changedBlocks_.begin(), changedBlocks_.end(), 0u);  // IN-453: every version is fresh below
            MarkWritten(BlockSpan{0, kAllBlocks});
            for (Target &t : targets_) {
                if (t.alive) PushDirty(t, Dirty{PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1,
                                                         static_cast<std::int32_t>(t.height) - 1},
                                               t.view.psm});
            }
            return;
        }
        if (swizzled && !changed && keepUnchanged_) {
            // IN-450: GT4 uploads most of its textures again every VBlank, byte for byte. When no byte changed and no
            // target lies over the written blocks (a target's pixels are newer than memory, so it must take the write),
            // the page versions stay as they were: the cached decodes, palette rows and GLM pages that read these
            // pages still hold exactly this memory. The upload is still remembered for targets created later (2.4b).
            const BlockSpan span = spanKnown ? writtenSpan : WriteSpan(view, rect);
            bool overTarget = false;
            for (const Target &t : targets_) {
                if (!t.alive) continue;
                const BlockSpan whole = TargetSpan(t, PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1,
                                                                static_cast<std::int32_t>(t.height) - 1});
                if (SpansOverlap(whole, span)) {
                    overTarget = true;
                    break;
                }
            }
            if (!overTarget) {
                stats.unchangedUploads++;
                RememberUpload(view, rect, span);
                return;
            }
        }
        OnMemoryWritten(view, rect, true, kWriteUpload, spanKnown ? &writtenSpan : nullptr);
        if (contentCacheOn_ && !replayed) SetUploadIds(view, rect, swizzled);  // IN-453 (block identities are unused when off)
        if (rememberThis && swizzled) RememberUploadBytes(memoKey, view, rect);  // IN-486
    }

    // IN-450: GT4_CORE2_KEEP_UNCHANGED=0 bumps the page versions for every upload (A/B).
    const bool keepUnchanged_ = std::getenv("GT4_CORE2_KEEP_UNCHANGED") == nullptr ||
                                std::strcmp(std::getenv("GT4_CORE2_KEEP_UNCHANGED"), "0") != 0;

    void LocalToLocalMove(const LocalMoveCommit &move) override {
        AuditIds("before move");
        NoteBaseWrite noteBaseWrite{*this, BaseWriteKey(move.dbp, move.dbw, move.dpsm)};
        const auto spsm = memory::decodePsm(move.spsm);
        const auto dpsm = memory::decodePsm(move.dpsm);
        if (spsm && move.rrw != 0 && move.rrh != 0) {
            const BufferView source{move.sbp, move.sbw, *spsm};
            const PixelRect rect{static_cast<std::int32_t>(move.ssax), static_cast<std::int32_t>(move.ssay),
                                 static_cast<std::int32_t>(move.ssax + move.rrw - 1u),
                                 static_cast<std::int32_t>(move.ssay + move.rrh - 1u)};
            if (!Readback(SpanOfRect(source, rect), &source, &rect)) return;
        } else if (!ReadbackAll()) {
            return;
        }
        if (dpsm && move.rrw != 0 && move.rrh != 0) {
            const PixelRect written{static_cast<std::int32_t>(move.dsax), static_cast<std::int32_t>(move.dsay),
                                    static_cast<std::int32_t>(move.dsax + move.rrw - 1u),
                                    static_cast<std::int32_t>(move.dsay + move.rrh - 1u)};
            if (!StoreDrawnBeforeWrite(BufferView{move.dbp, move.dbw, *dpsm}, written)) return;
        }
        memory_.LocalToLocalMove(move);
        if (dpsm && move.rrw != 0 && move.rrh != 0) {
            const BufferView destination{move.dbp, move.dbw, *dpsm};
            const PixelRect rect{static_cast<std::int32_t>(move.dsax), static_cast<std::int32_t>(move.dsay),
                                 static_cast<std::int32_t>(move.dsax + move.rrw - 1u),
                                 static_cast<std::int32_t>(move.dsay + move.rrh - 1u)};
            OnMemoryWritten(destination, rect, true, kWriteMove);
        } else {
            MarkWritten(BlockSpan{0, kAllBlocks});
        }
    }

    void BeginLocalToHost(const LocalToHostSetup &setup) override {
        pendingRead_ = setup;
        memory_.BeginLocalToHost(setup);
    }

    void CancelLocalToHost() override {
        pendingRead_.reset();
        zeroRead_ = false;
        memory_.CancelLocalToHost();
    }

    bool OnSync(SyncKind, std::uint32_t, std::uint32_t) override { return false; }

    // ---- 7.2 GT4's channel-shuffle hook ------------------------------------------------------------------------
    // A possible channel-shuffle draw (addendum: detection). Only the first draw of a run needs its texture base to
    // resolve to a colour target; later draws (GT4's copy-back strips read the temporary page) only need the shape.
    bool PossibleShuffle(const Batch &batch) const {
        const BatchState &state = batch.state;
        const std::uint64_t prim = state.effectivePrim;
        if (((prim >> primbit::kTme) & 1u) == 0) return false;
        if (batch.cls != PrimClass::Sprite) return false;
        const std::uint64_t tex0 = state.ctx.tex0;
        if (tex0::Psm(tex0) != 0x13u) return false;  // T8
        if (batch.vertices.size() < 2) return false;
        std::int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
        for (const Vertex &v : batch.vertices) {
            minX = std::min(minX, v.x4);
            minY = std::min(minY, v.y4);
            maxX = std::max(maxX, v.x4);
            maxY = std::max(maxY, v.y4);
        }
        const std::int32_t boxW = (maxX - minX) >> 4;
        const std::int32_t boxH = (maxY - minY) >> 4;
        if (batch.vertices.size() <= 2 && boxW <= 8 && boxH <= 8) return false;
        const std::int32_t firstW = std::abs(batch.vertices[1].x4 - batch.vertices[0].x4) >> 4;
        const std::int32_t firstH = std::abs(batch.vertices[1].y4 - batch.vertices[0].y4) >> 4;
        const std::uint64_t clamp = state.ctx.clamp;
        const bool regionMode = clampreg::Wms(clamp) >= 2 || clampreg::Wmt(clamp) >= 2;
        const bool shape = firstW == 8 || firstH == 2 || regionMode;
        if (!shape || boxW > 64) return false;
        // The strip case: a texture twice as wide as the frame, page aligned.
        if (boxH > 64 && tex0::Tbw(tex0) != 2u * frame::Fbw(state.ctx.frame)) return false;
        return true;
    }

    Target *ShuffleSource(const Batch &batch) {
        const std::uint32_t base = tex0::Tbp0(batch.state.ctx.tex0);
        Target *found = nullptr;
        for (Target &t : targets_) {
            if (!t.alive || t.cls != FormatClass::Colour32 || t.view.basePointer != base) continue;
            if (!found || t.lastDraw > found->lastDraw) found = &t;
        }
        return found;
    }

    // True when the batch belongs to a shuffle run and was replaced (skipped).
    bool ToneHook(const Batch &batch) {
        if (!PossibleShuffle(batch)) {
            toneActive_ = false;
            return false;
        }
        const std::uint32_t fbmsk = frame::Fbmsk(batch.state.ctx.frame);
        if (toneActive_ && fbmsk == toneFbmsk_) {
            stats.toneDrawsSkipped++;
            return true;
        }
        Target *target = ShuffleSource(batch);
        if (!target) {
            toneActive_ = false;
            return false;
        }
        if (fbmsk == 0x00FFFFFFu) {
            stats.toneAlphaOnlyUnsupported++;
            toneActive_ = false;
            return false;
        }
        // A new run: the palette as the first draw's CLUT load left it (CPSM, CSA, TEXA of this draw).
        const std::uint64_t tex0 = batch.state.ctx.tex0;
        if (!SyncClutToCpu(static_cast<std::uint32_t>(tex0::Psm(tex0)), static_cast<std::uint32_t>(tex0::Cpsm(tex0)),
                           static_cast<std::uint32_t>(tex0::Csa(tex0)))) {
            return true;
        }
        const clut::Palette palette =
            frontEnd_.Clut().Decode(tex0::Psm(tex0), tex0::Cpsm(tex0), tex0::Csa(tex0), batch.state.env.texa);
        if (!ApplyDirty(*target)) return true;
        const PixelRect extent = Union(target->valid, target->drawn);
        if (!extent.Empty()) {
            std::string error;
            if (!gpu_.Tone(target->image, static_cast<std::uint32_t>(extent.x1 + 1),
                           static_cast<std::uint32_t>(extent.y1 + 1), palette.entries, error)) {
                Fail(error);
                return true;
            }
            target->drawn = Union(target->drawn, PixelRect{0, 0, extent.x1, extent.y1});
            target->lastDraw = ++drawCounter_;
            target->usedThisFrame = true;
        }
        static const bool trace = std::getenv("GT4_CORE2_TONE_TRACE") != nullptr;  // diagnostics only
        if (trace) {
            std::fprintf(stderr,
                         "[core2] tone run vblank=%llu fbmsk=%08x frame=%u tex=%u target=%u extent=%d,%d cbp=%u "
                         "cpsm=%u csa=%u palette 0:%08x 64:%08x 128:%08x 192:%08x 255:%08x\n",
                         static_cast<unsigned long long>(vblanks_), fbmsk,
                         frame::Fbp(batch.state.ctx.frame) * 32u, tex0::Tbp0(tex0), target->view.basePointer,
                         extent.x1, extent.y1, static_cast<unsigned>(tex0::Cbp(tex0)),
                         static_cast<unsigned>(tex0::Cpsm(tex0)), static_cast<unsigned>(tex0::Csa(tex0)),
                         palette.entries[0], palette.entries[64], palette.entries[128], palette.entries[192],
                         palette.entries[255]);
        }
        toneActive_ = true;
        toneFbmsk_ = fbmsk;
        stats.toneRuns++;
        stats.toneDrawsSkipped++;
        counters.toneRuns++;
        return true;
    }

    // IN-407 (the old core's IN-348): GT4 draws its palettes as small untextured sprites and loads a CLUT from them
    // right away, about three times per VBlank in a night race. On the GPU each one became a target that the CLUT
    // load read back (a submit and a full wait), and the next texture upload over that memory replaced the target.
    // Such a batch is drawn by the CPU reference straight into GS memory instead, like an EE write: exact for
    // untextured sprites, and the CLUT load then reads memory with no readback. Only where no target holds content
    // (valid or drawn) in the memory the batch writes or tests: the CPU must see the newest pixels, and a small
    // sprite on a resident target (a HUD box on the frame buffer) stays a GPU draw at scale. The write is remembered
    // like an upload (2.4b), so a target created over that memory later starts from these pixels.
    // GT4_CORE2_CPU_SPRITES=0 turns it off (A/B).
    // IN-528: GT4_CORE2_SPRITE_IDS=0 keeps the blocks' old identities after a CPU sprite (A/B, to show the fault).
    const bool spriteFreshIds_ = std::getenv("GT4_CORE2_SPRITE_IDS") == nullptr ||
                                 std::strcmp(std::getenv("GT4_CORE2_SPRITE_IDS"), "0") != 0;
    const bool cpuSprites_ = std::getenv("GT4_CORE2_CPU_SPRITES") == nullptr ||
                             std::strcmp(std::getenv("GT4_CORE2_CPU_SPRITES"), "0") != 0;
    bool CpuSmallSprites(const Batch &batch, const writeback::BatchTranslation &t, bool depthUsed) const {
        if (!cpuSprites_ || t.hasTexture || t.rect.Empty() || batch.prims.empty()) return false;
        const std::int64_t area = static_cast<std::int64_t>(t.rect.x1 - t.rect.x0 + 1) * (t.rect.y1 - t.rect.y0 + 1);
        if (area > 64 * 64) return false;
        for (const PrimRecord &record : batch.prims) {
            if (record.type != PrimType::Sprite) return false;
        }
        const BlockSpan frameSpan = WriteSpan(t.frameView, t.rect);
        const BlockSpan depthSpan = depthUsed ? WriteSpan(t.zView, t.rect) : BlockSpan{};
        for (const Target &target : targets_) {
            if (!target.alive) continue;
            const PixelRect held = Union(target.valid, target.drawn);
            if (held.Empty()) continue;
            const BlockSpan heldSpan = WriteSpan(target.view, held);
            if (SpansOverlap(heldSpan, frameSpan) || SpansOverlap(heldSpan, depthSpan)) return false;
        }
        return true;
    }

    // D1 survey (diagnostics only, GT4_CORE2_D1_STATS=1): draw batches by the pixel-state fields that decide whether
    // fixed-function raster and blending can draw them, with the summed primitive box area (native pixels, an upper
    // bound of the fragments), printed with each [core2] line.
    const bool d1Stats_ = std::getenv("GT4_CORE2_D1_STATS") != nullptr;
    struct D1Class {
        std::uint64_t batches = 0, area = 0;
    };
    std::unordered_map<std::string, D1Class> d1Classes_;
    void NoteD1Class(const writeback::BatchTranslation &t, const DrawBatch &draw) {
        const auto &f = t.state.fields;
        namespace g = gpu::gpufield;
        const bool zRead = f[g::Ztest] != 0 && f[g::Ztst] != 1u;
        const bool zWrite = f[g::Zmsk] == 0;
        char key[160];
        std::snprintf(key, sizeof key,
                      "fmt=%u z=%s%s zf=%u blend=%s ate=%u/%u afail=%u date=%u pabe=%u clamp=%u fbmsk=%s dither=%u tex=%s",
                      f[g::Format], zRead ? "test" : "-", zWrite ? "+write" : "", f[g::Zformat],
                      f[g::BlendOn] ? (std::to_string(f[g::BlendA]) + std::to_string(f[g::BlendB]) +
                                       std::to_string(f[g::BlendC]) + std::to_string(f[g::BlendD]))
                                          .c_str()
                                    : "off",
                      f[g::Ate], f[g::Atst], f[g::Ate] ? f[g::Afail] : 0u, f[g::DateOn], f[g::Pabe], f[g::Colclamp],
                      f[g::Fbmsk] == 0 ? "0" : f[g::Fbmsk] == 0xFF000000u ? "A" : "other", f[g::Dither],
                      !t.hasTexture ? "-" : draw.texture != kNoImage ? "target" : "memory");
        D1Class &c = d1Classes_[key];
        c.batches++;
        for (const gpu::GpuDraw &d : t.draws) {
            const std::int64_t w = static_cast<std::int64_t>(d.x1) - d.x0 + 1, h = static_cast<std::int64_t>(d.y1) - d.y0 + 1;
            if (w > 0 && h > 0) c.area += static_cast<std::uint64_t>(w * h);
        }
    }
    void PrintD1Classes() {
        std::vector<std::pair<std::uint64_t, std::string>> rows;
        std::uint64_t total = 0;
        for (const auto &[key, c] : d1Classes_) {
            rows.push_back({c.area, key});
            total += c.area;
        }
        std::sort(rows.rbegin(), rows.rend());
        for (std::size_t i = 0; i < rows.size() && i < 30; ++i) {
            std::fprintf(stderr, "[core2] d1 %5.1f%% area=%llu batches=%llu %s\n",
                         total ? 100.0 * static_cast<double>(rows[i].first) / static_cast<double>(total) : 0.0,
                         static_cast<unsigned long long>(rows[i].first),
                         static_cast<unsigned long long>(d1Classes_[rows[i].second].batches), rows[i].second.c_str());
        }
        d1Classes_.clear();
    }

    // ---- Batches ---------------------------------------------------------------------------------------------
    writeback::BatchTranslation translation_;  // B4: reused by OnBatch
    std::uint32_t batchDepth_ = 0;

    // Empties a translation for the next batch, keeping the capacity of its vectors.
    static void ResetTranslation(writeback::BatchTranslation &t) {
        std::vector<std::uint32_t> palette, texels;
        std::vector<gpu::GpuVertex> vertices;
        std::vector<gpu::GpuDraw> draws;
        palette.swap(t.palette);
        texels.swap(t.texels);
        vertices.swap(t.vertices);
        draws.swap(t.draws);
        t = writeback::BatchTranslation{};
        palette.clear();
        texels.clear();
        vertices.clear();
        draws.clear();
        t.palette.swap(palette);
        t.texels.swap(texels);
        t.vertices.swap(vertices);
        t.draws.swap(draws);
    }

    // ---- Texture replacement and dumping (chapter 11, Format P: the names of existing packs) --------------------
    // IN-470: GT4_HW_TEXTURES=<folder> scans the folder recursively for replacement names (the launcher's flat Textures
    // folder). A batch's texture is looked up by what it holds: a cheap key and the page versions first, then the block
    // key, and only for new content the Format P hash over GS memory. A hit loads on the worker thread and the native
    // decode draws until the image arrives at a VBlank. GT4_HW_TEXTURES_SYNC=1 waits for the load instead (fixtures).
    // GT4_HW_DUMP_TEXTURES=1 writes every texture without a replacement to GT4_HW_TEXTURE_DUMP_DIR (default TextureDumps
    // beside the folder). GT4_HW_TEXTURES_MB sets the budget of replacement images on the GPU.
    struct ReplacedTexture {
        ImageId image = kNoImage;  // kNoImage: not on the GPU
        bool loading = false;
        bool failed = false;
        std::uint32_t width = 0, height = 0;
        std::uint32_t logicalX = 0, logicalY = 0, logicalWidth = 0, logicalHeight = 0;
        std::uint64_t bytes = 0;
        std::uint64_t lastUse = 0;
        hw_gs_replace::PackEntry entry;
    };
    struct ReplacementLookup {
        BlockSpan span;
        std::uint64_t sequence = 0;
        std::uint64_t id = 0;      // 0: no replacement
        bool triedUpload = false;  // the hash ran with an upload extent (chapter 04.6)
    };
    class SlmTexels final : public hw_gs_replace::TexelSource {
    public:
        explicit SlmTexels(const memory::LocalMemory &local) : local_(local) {}
        void ReadBlock(std::uint32_t blockNumber, std::span<std::uint8_t, 256> out) const override {
            std::memcpy(out.data(), local_.bytes().data() + static_cast<std::size_t>(blockNumber) * 256u, 256);
        }
        std::uint32_t ReadPixel(hw_gs_replace::Psm psm, std::uint32_t basePointer, std::uint32_t bufferWidth, std::uint32_t x,
                                std::uint32_t y) const override {
            return local_.readPixel(BufferView{basePointer, bufferWidth, static_cast<Psm>(psm)}, x, y);
        }
        void ReadRow(hw_gs_replace::Psm psm, std::uint32_t basePointer, std::uint32_t bufferWidth, std::uint32_t x,
                     std::uint32_t y, std::uint32_t count, std::uint32_t *out) const override {
            if (x + count <= 2048u && y < 2048u) {
                local_.readRect(BufferView{basePointer, bufferWidth, static_cast<Psm>(psm)}, x, y, count, 1, out);
            } else {
                hw_gs_replace::TexelSource::ReadRow(psm, basePointer, bufferWidth, x, y, count, out);
            }
        }

    private:
        const memory::LocalMemory &local_;
    };
    std::unique_ptr<ReplacementLoader> replacementLoader_;
    hw_gs_replace::ReplacementIndex replacementIndex_;
    bool replacementSync_ = false;
    bool textureDumps_ = false;
    std::filesystem::path textureDumpDir_;
    std::unordered_set<std::string> textureDumpsDone_;
    std::unordered_map<std::uint64_t, ReplacementLookup> replacementLookups_;    // by the cheap key
    std::unordered_map<std::uint64_t, ReplacementLookup> replacementByContent_;  // by the block key and the cheap key
    std::unordered_map<std::uint64_t, ReplacedTexture> replacedTextures_;        // by id
    std::unordered_map<std::string, std::uint64_t> replacedByFile_;              // one image per file
    std::uint64_t nextReplacementId_ = 1;
    std::uint64_t replacedBytes_ = 0;
    std::uint64_t replacedBudget_ = 0;
    std::uint64_t replacementUses_ = 0;
    std::deque<ReplacementLoader::Loaded> replacementArrivals_;

    void StartTextureReplacement() {
        // Read as Windows holds them: in the narrow form an accented folder is in the ANSI code page, not UTF-8.
        const wchar_t *root = _wgetenv(L"GT4_HW_TEXTURES");
        const char *dump = std::getenv("GT4_HW_DUMP_TEXTURES");
        textureDumps_ = dump != nullptr && std::strcmp(dump, "0") != 0;
        replacementSync_ = std::getenv("GT4_HW_TEXTURES_SYNC") != nullptr;
        const char *megabytes = std::getenv("GT4_HW_TEXTURES_MB");
        replacedBudget_ = megabytes != nullptr && std::atoll(megabytes) > 0
                              ? static_cast<std::uint64_t>(std::atoll(megabytes)) << 20
                              : static_cast<std::uint64_t>(gpu_.Integrated() ? 512 : 1536) << 20;
        const std::filesystem::path folder =
            root != nullptr && *root != 0 ? std::filesystem::path(root) : std::filesystem::path{};
        if (!folder.empty()) {
            hw_gs_replace::UnsupportedArchiveReader archives;
            const hw_gs_replace::ScanReport report = replacementIndex_.ScanFormatPFolder(folder, archives);
            std::fprintf(stderr, "[core2] texture replacement: %zu files registered, %zu duplicates, %zu other names\n",
                         report.filesRegistered, report.duplicatesIgnored, report.unparsedNames);
            stats.replacementFiles = report.filesRegistered;
        }
        if (textureDumps_) {
            const wchar_t *dumpDir = _wgetenv(L"GT4_HW_TEXTURE_DUMP_DIR");
            textureDumpDir_ = dumpDir != nullptr ? std::filesystem::path(dumpDir)
                                                 : (folder.empty() ? std::filesystem::path("TextureDumps")
                                                                   : folder.parent_path() / "TextureDumps");
        }
        if (stats.replacementFiles != 0 || textureDumps_) replacementLoader_ = std::make_unique<ReplacementLoader>();
    }

    // The extent the batch's vertices read (chapter 04.6), in texels.
    static void ReadExtentOf(const Batch &batch, std::uint64_t tex0, bool fst, std::uint32_t &width, std::uint32_t &height) {
        double maxU = 0.0, maxV = 0.0;
        const double sizeU = static_cast<double>(1u << tex0::Tw(tex0)), sizeV = static_cast<double>(1u << tex0::Th(tex0));
        for (const Vertex &v : batch.vertices) {
            if (fst) {
                maxU = std::max(maxU, v.u / 16.0);
                maxV = std::max(maxV, v.v / 16.0);
            } else if (v.q != 0.0f) {
                maxU = std::max(maxU, static_cast<double>(v.s) / v.q * sizeU);
                maxV = std::max(maxV, static_cast<double>(v.t) / v.q * sizeV);
            }
        }
        width = static_cast<std::uint32_t>(std::min(std::ceil(maxU), sizeU));
        height = static_cast<std::uint32_t>(std::min(std::ceil(maxV), sizeV));
    }

    // Chapter 11.6: the texture with its palette applied, as RGBA8, once per name.
    void DumpTexture(const hw_gs_replace::TexelSource &source, const hw_gs_replace::TextureDescription &desc,
                     const hw_gs_replace::FormatPKeys &keys) {
        namespace rp = hw_gs_replace;
        const std::string name = rp::FormatPDumpName(keys.fullChain, rp::ImageExtension::Png);
        if (!textureDumpsDone_.insert(name).second) return;
        const rp::Region regionX = rp::ResolveRegion(desc.wrapU, desc.tw), regionY = rp::ResolveRegion(desc.wrapV, desc.th);
        const std::uint32_t x0 = regionX.present ? regionX.minimum : 0u, y0 = regionY.present ? regionY.minimum : 0u;
        const std::uint32_t width = keys.fullChain.regionWidth != 0 ? keys.fullChain.regionWidth : 1u << desc.tw;
        const std::uint32_t height = keys.fullChain.regionHeight != 0 ? keys.fullChain.regionHeight : 1u << desc.th;
        std::vector<std::uint32_t> palette;
        if (rp::IsIndexedPsm(desc.psm)) {
            const bool eightBit = desc.psm == rp::Psm::T8 || desc.psm == rp::Psm::T8H;
            palette = rp::DecodeClut(*desc.clut, desc.clutPsm, desc.csa, eightBit ? 256u : 16u, desc.texa);
        }
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4u);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint32_t stored = source.ReadPixel(desc.psm, desc.tbp, desc.tbw, x0 + x, y0 + y);
                const std::uint32_t word = !palette.empty() ? palette[stored & (static_cast<std::uint32_t>(palette.size()) - 1u)]
                                                            : rp::ExpandDirectTexel(desc.psm, stored, desc.texa);
                std::memcpy(rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4u, &word, 4);
            }
        }
        replacementLoader_->Dump(textureDumpDir_ / name, width, height, std::move(rgba));
        stats.replacementDumps++;
    }

    // Puts finished images on the GPU; at a VBlank at most kReplacementUploadBytes of them (the rest at the next ones).
    static constexpr std::uint64_t kReplacementUploadBytes = static_cast<std::uint64_t>(48) << 20;
    void PumpReplacements(bool everything) {
        if (!replacementLoader_) return;
        ReplacementLoader::Loaded loaded;
        while (replacementLoader_->Take(loaded)) replacementArrivals_.push_back(std::move(loaded));
        std::uint64_t uploaded = 0;
        while (!replacementArrivals_.empty() && (everything || uploaded < kReplacementUploadBytes)) {
            ReplacementLoader::Loaded &arrival = replacementArrivals_.front();
            auto it = replacedTextures_.find(arrival.id);
            if (it != replacedTextures_.end()) {
                ReplacedTexture &texture = it->second;
                texture.loading = false;
                std::string error = arrival.error;
                if (error.empty() && arrival.rgba.size() != std::size_t{arrival.width} * arrival.height * 4u) {
                    error = "unexpected image size";
                }
                ImageId image = kNoImage;
                if (error.empty()) image = gpu_.CreateImage(arrival.width, arrival.height, false, error);
                if (image != kNoImage && !gpu_.UploadImage(image, arrival.width, arrival.height,
                                                           reinterpret_cast<const std::uint32_t *>(arrival.rgba.data()), error)) {
                    Fail(error);
                    return;
                }
                if (!error.empty()) {
                    std::fprintf(stderr, "[core2] texture replacement %s refused: %s\n", texture.entry.basename.c_str(),
                                 error.c_str());
                    texture.failed = true;
                    stats.replacementFailed++;
                } else {
                    texture.image = image;
                    texture.width = arrival.width;
                    texture.height = arrival.height;
                    texture.bytes = arrival.rgba.size();
                    texture.lastUse = ++replacementUses_;
                    replacedBytes_ += texture.bytes;
                    uploaded += texture.bytes;
                    stats.replacementLoads++;
                    stats.replacementBytes = replacedBytes_;
                    EvictReplacements(it->first);
                }
            }
            replacementArrivals_.pop_front();
        }
    }

    // Least recently used images leave the GPU past the budget; a later use loads them again.
    void EvictReplacements(std::uint64_t keep) {
        while (replacedBytes_ > replacedBudget_) {
            ReplacedTexture *oldest = nullptr;
            for (auto &[id, texture] : replacedTextures_) {
                if (id == keep || texture.image == kNoImage) continue;
                if (!oldest || texture.lastUse < oldest->lastUse) oldest = &texture;
            }
            if (!oldest) return;
            gpu_.DestroyImage(oldest->image);
            oldest->image = kNoImage;
            replacedBytes_ -= std::min(replacedBytes_, oldest->bytes);
            stats.replacementBytes = replacedBytes_;
        }
    }

    // The batch's replacement image, or null: none, not eligible, or still loading (the native decode draws).
    const ReplacedTexture *FindReplacement(const Batch &batch, const writeback::BatchTranslation &t) {
        namespace rp = hw_gs_replace;
        if (!replacementLoader_ || !t.glmTexture) return nullptr;
        ScopedUs timer(clock_, &PhaseTimes::texture);
        const std::uint64_t tex0 = batch.state.ctx.tex0, texa = batch.state.env.texa, clamp = batch.state.ctx.clamp;
        const std::uint64_t tex1 = batch.state.ctx.tex1;
        // Chapter 11.1: memory sources only. A palette written on the GPU is unknown here, so it cannot be hashed.
        if (t.glmIndexed && GpuPaletteKey(t.glmPsm, tex0::Cpsm(tex0), tex0::Csa(tex0)) != 0) return nullptr;
        const auto texturePsm = memory::decodePsm(t.glmPsm);
        if (!texturePsm) return nullptr;
        namespace gf = gpu::gpufield;
        const auto &fields = t.state.fields;
        const std::uint32_t tw = tex0::Tw(tex0), th = tex0::Th(tex0);
        const rp::WrapAxis wrapU{static_cast<rp::WrapMode>(fields[gf::WrapS]), fields[gf::WrapMinS], fields[gf::WrapMaxS]};
        const rp::WrapAxis wrapV{static_cast<rp::WrapMode>(fields[gf::WrapT]), fields[gf::WrapMinT], fields[gf::WrapMaxT]};
        // Chapter 04.6: an upload may shrink a clamp-clamp texture to what the batch reads.
        std::uint32_t readWidth = 0, readHeight = 0;
        const bool inferable = fields[gf::WrapS] == 1u && fields[gf::WrapT] == 1u;
        if (inferable) ReadExtentOf(batch, tex0, fields[gf::TexFst] != 0u, readWidth, readHeight);

        std::uint64_t key = MixWords(MixWords(0, tex0 & ((std::uint64_t{1} << 34) - 1u)), clamp);
        key = MixWords(MixWords(key, tex1), batch.state.ctx.miptbp1);
        key = MixWords(MixWords(key, batch.state.ctx.miptbp2), texa);
        key = MixWords(key, (std::uint64_t{readWidth} << 32) | readHeight);
        if (t.glmIndexed) key = MixWords(key, writeback::HashPaletteWords(t.palette));

        std::uint64_t id = 0;
        auto lookup = replacementLookups_.find(key);
        if (lookup != replacementLookups_.end() && UnchangedSince(lookup->second.span, lookup->second.sequence)) {
            id = lookup->second.id;
        } else {
            const MipPlan mip = PlanMips(t, tex1, batch.state.ctx.miptbp1, batch.state.ctx.miptbp2);
            writeback::TextureKey address;
            address.tbp = t.glmBp;
            address.tbw = t.glmBw;
            address.psm = t.glmPsm;
            address.width = mip.levels > 0 ? mip.imageWidth : t.texWidth;
            address.height = mip.levels > 0 ? mip.imageHeight : t.texHeight;
            address.mipHash = mip.hash;
            BlockSpan span = SpanOfRect(BufferView{t.glmBp, t.glmBw, *texturePsm}, 0, 0, static_cast<std::int32_t>(t.texWidth) - 1,
                                        static_cast<std::int32_t>(t.texHeight) - 1);
            for (std::uint32_t n = 1; n <= mip.levels; ++n) {
                span = UnionSpans(span, SpanOfRect(BufferView{mip.address[n].tbp, mip.address[n].tbw, *texturePsm}, 0, 0,
                                                   static_cast<std::int32_t>(mip.size[n].width) - 1,
                                                   static_cast<std::int32_t>(mip.size[n].height) - 1));
            }
            const std::uint64_t content = MixWords(ContentKey(KeyHash(address), t, *texturePsm, mip), key);
            // The upload of chapter 04.6, in the last 10 batches.
            std::optional<rp::UploadExtent> upload;
            if (inferable) {
                for (auto u = recentUploads_.rbegin(); u != recentUploads_.rend() && drawCounter_ - u->drawNumber <= 10u; ++u) {
                    if (u->view.basePointer == t.glmBp && u->view.bufferWidth == t.glmBw && u->view.psm == *texturePsm) {
                        upload = rp::UploadExtent{static_cast<std::uint32_t>(u->rect.x1 + 1),
                                                  static_cast<std::uint32_t>(u->rect.y1 + 1)};
                        break;
                    }
                }
            }
            auto known = replacementByContent_.find(content);
            ReplacementLookup result;
            if (known != replacementByContent_.end() && (known->second.id != 0 || known->second.triedUpload || !upload)) {
                result = known->second;
            } else {
                result = HashAndFind(t, tex0, texa, wrapU, wrapV, tw, th, mip, upload, readWidth, readHeight);
                replacementByContent_[content] = result;
            }
            result.span = span;
            result.sequence = writeSequence_;
            replacementLookups_[key] = result;
            id = result.id;
        }
        if (id == 0) return nullptr;
        ReplacedTexture &texture = replacedTextures_[id];
        if (texture.image == kNoImage) {
            if (!texture.loading && !texture.failed) {
                texture.loading = true;
                replacementLoader_->Request(id, texture.entry);
                stats.replacementRequests++;
            }
            if (replacementSync_ && texture.loading) {
                ReplacementLoader::Loaded loaded;
                if (replacementLoader_->Take(loaded, true)) replacementArrivals_.push_back(std::move(loaded));
                PumpReplacements(true);
            }
            if (texture.image == kNoImage) return nullptr;
        }
        texture.lastUse = ++replacementUses_;
        return &texture;
    }

    // Format P key over GS memory, then the pack index.
    ReplacementLookup HashAndFind(const writeback::BatchTranslation &t, std::uint64_t tex0, std::uint64_t texa,
                                  const hw_gs_replace::WrapAxis &wrapU, const hw_gs_replace::WrapAxis &wrapV, std::uint32_t tw,
                                  std::uint32_t th, const MipPlan &mip, const std::optional<hw_gs_replace::UploadExtent> &upload,
                                  std::uint32_t readWidth, std::uint32_t readHeight) {
        namespace rp = hw_gs_replace;
        ReplacementLookup result;
        result.triedUpload = upload.has_value();
        const rp::Region regionX = rp::ResolveRegion(wrapU, tw), regionY = rp::ResolveRegion(wrapV, th);
        // Chapter 11.1: a texture over 1024 texels on an axis without a region is never replaced.
        if ((tw > 10u && !regionX.present) || (th > 10u && !regionY.present)) return result;
        if (!rp::IsKnownPsmCode(t.glmPsm) || !rp::IsKnownPsmCode(tex0::Cpsm(tex0))) return result;
        rp::ClutBuffer clut;
        std::memcpy(clut.halfwords.data(), frontEnd_.Clut().Halfwords(), sizeof clut.halfwords);
        rp::TextureDescription desc;
        desc.psm = rp::PsmFromCode(t.glmPsm);
        desc.texa = rp::TexaState{static_cast<std::uint8_t>(texa & 0xFFu), ((texa >> 15) & 1u) != 0,
                                  static_cast<std::uint8_t>((texa >> 32) & 0xFFu)};
        desc.wrapU = wrapU;
        desc.wrapV = wrapV;
        desc.tw = tw;
        desc.th = th;
        desc.tbp = t.glmBp;
        desc.tbw = t.glmBw;
        desc.clut = &clut;
        desc.clutPsm = rp::PsmFromCode(tex0::Cpsm(tex0));
        desc.csa = tex0::Csa(tex0);
        for (std::uint32_t n = 1; n <= mip.levels; ++n) {
            desc.mipLevels.push_back(rp::MipLevelAddress{mip.address[n].tbp, mip.address[n].tbw});
        }
        desc.mipmappingActive = mip.levels > 0;
        desc.exactUploadInLast10Batches = upload;
        desc.readExtent = rp::UploadExtent{readWidth, readHeight};
        const SlmTexels source(memory_.Local());
        const rp::FormatPKeys keys = rp::ComputeFormatPKeys(source, desc);
        stats.replacementKeys++;
        std::optional<rp::PackEntry> entry = replacementIndex_.FindFormatP(keys.fullChain);
        if (!entry && keys.baseOnly) entry = replacementIndex_.FindFormatP(*keys.baseOnly);
        if (!entry) {
            if (textureDumps_) DumpTexture(source, desc, keys);
            return result;
        }
        const std::u8string fileText = entry->file.u8string();
        const std::string file(reinterpret_cast<const char *>(fileText.data()), fileText.size());
        auto existing = replacedByFile_.find(file);
        if (existing != replacedByFile_.end()) {
            result.id = existing->second;
            return result;
        }
        result.id = nextReplacementId_++;
        replacedByFile_[file] = result.id;
        ReplacedTexture &texture = replacedTextures_[result.id];
        texture.entry = std::move(*entry);
        texture.logicalX = regionX.present ? regionX.minimum : 0u;
        texture.logicalY = regionY.present ? regionY.minimum : 0u;
        texture.logicalWidth = keys.fullChain.regionWidth != 0 ? keys.fullChain.regionWidth : 1u << tw;
        texture.logicalHeight = keys.fullChain.regionHeight != 0 ? keys.fullChain.regionHeight : 1u << th;
        return result;
    }

    // IN-529: GT4 clears the depth buffer it is drawing with through a pair of views of its memory: untextured black sprites of z 0
    // on the even page columns, with FRAME at the depth buffer's base as CT24 and ZBUF one page after it. The FRAME write lands on
    // the even columns of the depth memory and the ZBUF write on the odd ones, so all of it reads 0 (it splits the scene in two depth
    // ranges). Depth and colour targets of different bases do not meet here, so the clear is drawn as what it is: a write of 0 to
    // the existing depth target over both columns, keeping every lane of the frame the scene is using.
    // GT4_CORE2_DEPTH_CLEAR=0 draws it as written (A/B).
    const bool depthClearOn_ = std::getenv("GT4_CORE2_DEPTH_CLEAR") == nullptr ||
                               std::strcmp(std::getenv("GT4_CORE2_DEPTH_CLEAR"), "0") != 0;
    std::uint64_t lastSceneFrame_ = 0;

    bool RewriteDepthClear(Batch &batch) {
        constexpr std::int32_t kPageWidth = 64;
        const std::uint64_t frameRegister = batch.state.ctx.frame, zbufRegister = batch.state.ctx.zbuf, testRegister = batch.state.ctx.test;
        if (!depthClearOn_ || batch.prims.empty() || lastSceneFrame_ == 0) return false;
        if ((batch.state.effectivePrim & ((1u << primbit::kTme) | (1u << primbit::kAbe))) != 0) return false;
        if (test::Ate(testRegister) != 0 || test::Date(testRegister) != 0 || test::Zte(testRegister) != 1 || test::Ztst(testRegister) != 1u) return false;
        if (frame::Psm(frameRegister) != static_cast<std::uint32_t>(memory::Psm::Ct24) || frame::Fbmsk(frameRegister) != 0) return false;
        if (zbuf::Zmsk(zbufRegister) != 0 || zbuf::Zbp(zbufRegister) != frame::Fbp(frameRegister) + 1u) return false;
        for (const PrimRecord &record : batch.prims) {
            if (record.type != PrimType::Sprite || record.rect.x0 % (2 * kPageWidth) != 0 || record.rect.x1 - record.rect.x0 + 1 != kPageWidth) return false;
        }
        for (const Vertex &v : batch.vertices) {
            if (v.z != 0 || v.r != 0 || v.g != 0 || v.b != 0) return false;
        }
        const std::uint32_t base = memory::blockOfPage(frame::Fbp(frameRegister));
        const std::uint32_t width = frame::Fbw(frameRegister);
        const bool haveDepth = std::any_of(targets_.begin(), targets_.end(), [&](const Target &t) {
            return t.alive && IsDepthClass(t.cls) && t.view.basePointer == base && t.view.bufferWidth == width;
        });
        if (!haveDepth) return false;
        batch.state.ctx.zbuf = (zbufRegister & ~std::uint64_t{0x1FF}) | frame::Fbp(frameRegister);
        batch.state.ctx.frame = lastSceneFrame_ | (std::uint64_t{0xFFFFFFFFu} << 32);
        for (PrimRecord &record : batch.prims) {
            batch.vertices[record.first + record.count - 1].x4 += kPageWidth * 16;
            record.rect.x1 += kPageWidth;
        }
        batch.drawRect.x1 += kPageWidth;
        batch.trace.xMax += kPageWidth;
        stats.depthClears++;
        return true;
    }

    void OnBatch(Batch &&batch) override {
        ScopedUs batchTimer(clock_, &PhaseTimes::batch);
        if (failed_) return;
        counters.batches++;
        if (!RewriteDepthClear(batch) && frame::Fbmsk(batch.state.ctx.frame) != 0xFFFFFFFFu) lastSceneFrame_ = batch.state.ctx.frame;
        {
            ScopedUs timer(clock_, &PhaseTimes::hook);
            if (ToneHook(batch)) return;
        }

        // B4: the translation's vectors keep their capacity from batch to batch (a nested batch, if one ever came while
        // this one is translated, gets its own).
        writeback::BatchTranslation nested;
        writeback::BatchTranslation &t = batchDepth_ == 0 ? translation_ : nested;
        ResetTranslation(t);
        ++batchDepth_;
        struct DepthGuard {
            std::uint32_t &depth;
            ~DepthGuard() { --depth; }
        } depthGuard{batchDepth_};
        textureHit_ = kNoImage;
        textureTarget_ = -1;
        pendingStore_ = false;
        const writeback::TextureFromTargetHook fromTarget = [this](const BufferView &view, std::uint32_t,
                                                                   std::uint32_t, const PixelRect &footprint) {
            Target *target = TextureTarget(view, footprint);
            if (!target) return -1;
            textureTarget_ = static_cast<std::int32_t>(target - targets_.data());
            textureFootprint_ = footprint;
            return textureTarget_;
        };
        // 3.7: every texture that is not a target hit is decoded from GS memory on the GPU (the GLM). Interim 3.5: an
        // indexed texture over a drawn target reads memory, so that target's drawn area is reduced into memory first
        // (the reference reduces it on the GPU at 1x; the result is the same).
        const writeback::TextureFromGlmHook fromGlm = [this](const BufferView &view, std::uint32_t width,
                                                             std::uint32_t height, bool indexed, bool &gpuClut) {
            ScopedUs hookTimer(clock_, &PhaseTimes::glmHook);
            gpuClut = false;
            static const bool readAll = std::getenv("GT4_CORE2_TEXTURE_READBACK") == nullptr ||
                                        std::strcmp(std::getenv("GT4_CORE2_TEXTURE_READBACK"), "0") != 0;
            static const bool indexedReadback = std::getenv("GT4_CORE2_INDEXED_READBACK") == nullptr ||
                                                std::strcmp(std::getenv("GT4_CORE2_INDEXED_READBACK"), "0") != 0;
            if ((indexed && indexedReadback) || (!indexed && readAll)) {
                const PixelRect rect{0, 0, static_cast<std::int32_t>(width) - 1,
                                     static_cast<std::int32_t>(height) - 1};
                const BlockSpan span = SpanOfRect(view, rect);
                std::optional<BlockSpan> exact;
                static const bool trace = std::getenv("GT4_CORE2_READBACK_TRACE") != nullptr;
                for (const Target &target : targets_) {
                    // IN-518: a target that needs no readback costs no block walk (0.45 ms per 6x race VBlank, once per
                    // texture for every target).
                    if (gpuTextures_ && MaterializableClass(target.cls) && !trace) continue;
                    if (!target.alive || target.drawn.Empty() || !SpansOverlap(TargetSpan(target, target.drawn), span)) continue;
                    // The page-row test first (cheap), then exact blocks: a conservative overlap alone would read back
                    // targets the texture does not touch.
                    if (!exact) exact = WriteSpan(view, rect);
                    if (SpansOverlap(WriteSpan(target.view, target.drawn), *exact)) {
                        if (trace) {
                            std::fprintf(stderr, "[core2] texture over target vblank=%llu tex bp=%u bw=%u psm=%u %ux%u\n",
                                         static_cast<unsigned long long>(vblanks_), view.basePointer, view.bufferWidth,
                                         static_cast<unsigned>(view.psm), width, height);
                        }
                        if (gpuTextures_ && MaterializableClass(target.cls)) continue;  // IN-408/417: MemoryTexture
                        if (indexed) {
                            stats.indexedFromTargetReadbacks++;
                        } else {
                            stats.directFromTargetReadbacks++;
                        }
                        Readback(span);
                        break;
                    }
                }
            }
            return true;
        };
        {
            ScopedUs timer(clock_, &PhaseTimes::translate);
            writeback::TranslateBatchHooked(batch, memory_.Local(), frontEnd_.Clut(), t, counters, {}, this, fromTarget,
                                            fromGlm);
        }
        if (t.skippedTexture) {
            counters.batchesSkippedTexture++;
            return;
        }
        if (t.draws.empty()) {
            counters.batchesEmpty++;
            return;
        }
        if (failed_) return;

        // Diagnostics: GT4_CORE2_PRIMID_TEX=<texture base block> draws every batch with that texture untextured, each
        // primitive in a colour that encodes its index (R = index & 255, G = index >> 8, B = 0x80), so a frame shows
        // which primitive wrote each sample. Depth and the alpha test (vertex alpha) still run.
        {
            static const long long idTex =
                std::getenv("GT4_CORE2_PRIMID_TEX") ? std::atoll(std::getenv("GT4_CORE2_PRIMID_TEX")) : -1;
            if (idTex >= 0 && t.hasTexture && static_cast<long long>(tex0::Tbp0(batch.state.ctx.tex0)) == idTex) {
                for (std::size_t i = 0; i < t.draws.size(); ++i) {
                    const gpu::GpuDraw &d = t.draws[i];
                    for (std::uint32_t v = d.first; v < d.first + d.count && v < t.vertices.size(); ++v) {
                        t.vertices[v].rgba[0] = static_cast<std::uint8_t>(i & 0xFFu);
                        t.vertices[v].rgba[1] = static_cast<std::uint8_t>((i >> 8) & 0xFFu);
                        t.vertices[v].rgba[2] = 0x80;
                        t.vertices[v].rgba[3] = 128;
                    }
                }
                t.state.fields[gpu::gpufield::TexOn] = 0;
                t.state.fields[gpu::gpufield::FogOn] = 0;
                t.state.fields[gpu::gpufield::BlendOn] = 0;
                t.state.fields[gpu::gpufield::Dither] = 0;
                t.hasTexture = false;
            }
        }

        const bool depthUsed = [&] {
            const auto &st = t.state;
            const bool testReads = st.fields[gpu::gpufield::Ztest] != 0 && st.fields[gpu::gpufield::Ztst] != 1u;
            return testReads || st.fields[gpu::gpufield::Zmsk] == 0;
        }();
        bool cpuSprite = false;
        {
            ScopedUs timer(clock_, &PhaseTimes::spriteCheck);
            cpuSprite = CpuSmallSprites(batch, t, depthUsed);
        }
        if (cpuSprite) {
            ScopedUs timer(clock_, &PhaseTimes::cpuSprites);
            cpuref::DrawStats drawStats;
            cpuref::DrawBatch(batch, memory_.Local(), frontEnd_.Clut(), drawStats);
            // Not an upload: SetUploadIds does not run, so the written blocks need fresh identities. Otherwise a later replay
            // of an upload (IN-486) takes the old identity for the old bytes, and skips restoring them (IN-528).
            const WriteKind spriteWrite = spriteFreshIds_ ? kWriteWhole : kWriteUpload;
            OnMemoryWritten(t.frameView, t.rect, true, spriteWrite);
            if (t.state.fields[gpu::gpufield::Zmsk] == 0) OnMemoryWritten(t.zView, t.rect, true, spriteWrite);
            stats.cpuSpriteBatches++;
            return;
        }

        {
            static const bool smallTrace = std::getenv("GT4_CORE2_SMALL_TRACE") != nullptr;  // diagnostics only
            const std::int64_t area =
                t.rect.Empty() ? 0 : static_cast<std::int64_t>(t.rect.x1 - t.rect.x0 + 1) * (t.rect.y1 - t.rect.y0 + 1);
            if (smallTrace && area > 0 && area <= 64 * 192) {
                unsigned primMask = 0;
                for (const PrimRecord &record : batch.prims) primMask |= 1u << static_cast<unsigned>(record.type);
                const char *holder = "none";
                const BlockSpan frameSpan = WriteSpan(t.frameView, t.rect);
                const BlockSpan depthSpan = depthUsed ? WriteSpan(t.zView, t.rect) : BlockSpan{};
                for (const Target &target : targets_) {
                    if (!target.alive) continue;
                    const PixelRect held = Union(target.valid, target.drawn);
                    if (held.Empty()) continue;
                    const BlockSpan heldSpan = WriteSpan(target.view, held);
                    if (SpansOverlap(heldSpan, frameSpan)) holder = "frame";
                    if (SpansOverlap(heldSpan, depthSpan)) holder = "depth";
                }
                std::fprintf(stderr,
                             "[core2] small vblank=%llu fb=%u fbw=%u psm=%u rect=%d,%d,%d,%d prims=%zu mask=%x tex=%d "
                             "tbp=%u depth=%d held=%s\n",
                             static_cast<unsigned long long>(vblanks_), t.frameView.basePointer,
                             t.frameView.bufferWidth, static_cast<unsigned>(t.frameView.psm), t.rect.x0, t.rect.y0,
                             t.rect.x1, t.rect.y1, batch.prims.size(), primMask, t.hasTexture ? 1 : 0,
                             static_cast<unsigned>(tex0::Tbp0(batch.state.ctx.tex0)), depthUsed ? 1 : 0, holder);
            }
        }
        ScopedUs targetsTimer(clock_, &PhaseTimes::targets);
        Target *frameTarget = DrawTarget(t.frameView, t.rect);
        if (!frameTarget) {
            counters.batchesEmpty++;
            return;
        }
        const std::size_t frameIndex = static_cast<std::size_t>(frameTarget - targets_.data());
        Target *depthTarget = depthUsed ? DrawTarget(t.zView, t.rect) : nullptr;
        frameTarget = &targets_[frameIndex];  // DrawTarget may have grown the table
        if (depthUsed && !depthTarget) return;
        if (!ApplyDirty(*frameTarget) || (depthTarget && !ApplyDirty(*depthTarget))) return;
        Target *textureTarget = textureTarget_ >= 0 ? &targets_[static_cast<std::size_t>(textureTarget_)] : nullptr;
        if (textureTarget && textureTarget->alive && !ApplyDirty(*textureTarget)) return;

        targetsTimer.Stop();
        DrawBatch draw;
        draw.frame = frameTarget->image;
        draw.depth = depthTarget ? depthTarget->image : kNoImage;
        draw.texture = textureTarget && textureTarget->alive ? textureTarget->image : kNoImage;
        draw.state = t.state;
        draw.vertices = &t.vertices;
        draw.draws = &t.draws;
        draw.forceLinear = forcedBilinear_ == 1 || (forcedBilinear_ == 2 && batch.cls != PrimClass::Sprite);
        {  // IN-442: an untextured sprite batch without blend, fog or DATE may be a constant fill (Gpu::Draw decides)
            namespace gf = gpu::gpufield;
            const auto &fields = t.state.fields;
            draw.constantFill = !t.hasTexture && batch.cls == PrimClass::Sprite && fields[gf::BlendOn] == 0 &&
                                fields[gf::FogOn] == 0 && fields[gf::DateOn] == 0 && (batch.state.env.scanmsk & 2u) == 0;
        }
        draw.scanmsk = static_cast<std::uint32_t>(batch.state.env.scanmsk);
        if (draw.forceLinear && t.hasTexture) stats.forcedBilinearBatches++;
        const std::uint32_t zFormat = t.state.fields[gpu::gpufield::Zformat];
        draw.zMax = zFormat == 1 ? 0xFFFFFFu : zFormat == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        const ReplacedTexture *replaced =
            t.hasTexture && draw.texture == kNoImage ? FindReplacement(batch, t) : nullptr;
        if (replaced) {  // IN-470: chapter 11, a replacement instead of the decode
            draw.memoryTexture = replaced->image;
            draw.replacedWidth = replaced->width;
            draw.replacedHeight = replaced->height;
            draw.logicalX = replaced->logicalX;
            draw.logicalY = replaced->logicalY;
            draw.logicalWidth = replaced->logicalWidth;
            draw.logicalHeight = replaced->logicalHeight;
            stats.replacementHits++;
        } else if (t.hasTexture && draw.texture == kNoImage) {
            if (!t.glmTexture) return;  // every memory texture is decoded on the GPU (the hook above)
            draw.memoryTexture = MemoryTexture(t, batch.state.ctx.tex0, batch.state.env.texa, batch.state.ctx.tex1,
                                               batch.state.ctx.miptbp1, batch.state.ctx.miptbp2, draw.mipInfo);
            if (draw.memoryTexture == kNoImage) return;
            if (t.glmIndexed && paletteRows_) {
                draw.paletteRow = PaletteRow(t, batch.state.ctx.tex0, batch.state.env.texa);
                if (draw.paletteRow < 0) return;
                draw.palettes = paletteImage_;
            }
            stats.texturesFromMemory++;
            // Diagnostics: GT4_CORE2_DUMP_TEX=<texture base block>:<folder> writes that texture as this core decoded
            // it (RGB as PPM, alpha as PGM) once, with its TEX0 and CLAMP.
            static long long dumpTex = -2;
            static std::string dumpDir;
            if (dumpTex == -2) {
                dumpTex = -1;
                if (const char *text = std::getenv("GT4_CORE2_DUMP_TEX")) {
                    if (const char *colon = std::strchr(text, ':')) {
                        dumpTex = std::atoll(text);
                        dumpDir = colon + 1;
                    }
                }
            }
            if (dumpTex >= 0 && static_cast<long long>(t.glmBp) == dumpTex) {
                dumpTex = -1;
                std::vector<std::uint32_t> words;
                std::string error;
                if (gpu_.ReadImage(draw.memoryTexture, t.texWidth, t.texHeight, words, error)) {
                    const std::string rgb = dumpDir + "\\texture.ppm", alpha = dumpDir + "\\texture-alpha.pgm";
                    if (std::FILE *f = std::fopen(rgb.c_str(), "wb")) {
                        std::fprintf(f, "P6\n%u %u\n255\n", t.texWidth, t.texHeight);
                        for (std::uint32_t w : words) {
                            const unsigned char px[3] = {static_cast<unsigned char>(w), static_cast<unsigned char>(w >> 8),
                                                         static_cast<unsigned char>(w >> 16)};
                            std::fwrite(px, 1, 3, f);
                        }
                        std::fclose(f);
                    }
                    if (std::FILE *f = std::fopen(alpha.c_str(), "wb")) {
                        std::fprintf(f, "P5\n%u %u\n255\n", t.texWidth, t.texHeight);
                        for (std::uint32_t w : words) {
                            const unsigned char a = static_cast<unsigned char>(w >> 24);
                            std::fwrite(&a, 1, 1, f);
                        }
                        std::fclose(f);
                    }
                    std::fprintf(stderr, "[core2] texture %u dumped: %ux%u tex0=%llx clamp=%llx texa=%llx\n", t.glmBp,
                                 t.texWidth, t.texHeight, static_cast<unsigned long long>(batch.state.ctx.tex0),
                                 static_cast<unsigned long long>(batch.state.ctx.clamp),
                                 static_cast<unsigned long long>(batch.state.env.texa));
                }
            }
        } else if (draw.texture != kNoImage) {
            stats.texturesFromTargets++;
            textureTarget->usedThisFrame = true;
            // Spec 8.8: a texture that is the colour or depth target this batch draws into, sampled where the batch
            // draws (GT4's glow buffers and luminance reductions, about 35 batches per race VBlank): the batch samples
            // a copy of the sampled rectangle taken before it (the auto-flush of 2.6 already splits a batch where a
            // later primitive must see an earlier one's pixels). Sampling the target itself raced with the batch's own
            // writes, so 12x frames differed from run to run. GT4_CORE2_SELF_COPY=0 samples the target itself (A/B).
            // IN-495: a batch that keeps every lane of its frame and samples that frame reads pixels nothing changes.
            namespace gf = gpu::gpufield;
            const bool frameUntouched = draw.texture == draw.frame && draw.texture != draw.depth &&
                                        t.state.fields[gf::Fbmsk] == 0xFFFFFFFFu;
            if (selfCopy_ && !frameUntouched && (draw.texture == draw.frame || draw.texture == draw.depth) &&
                !Intersect(textureFootprint_, t.rect).Empty()) {
                const PixelRect copy = Intersect(textureFootprint_,
                                                 PixelRect{0, 0, static_cast<std::int32_t>(textureTarget->width) - 1,
                                                           static_cast<std::int32_t>(textureTarget->height) - 1});
                draw.selfCopy = !copy.Empty();
                draw.copyX0 = copy.x0;
                draw.copyY0 = copy.y0;
                draw.copyX1 = copy.x1;
                draw.copyY1 = copy.y1;
            }
        }
        // Diagnostics: GT4_CORE2_SELF_TRACE=<vblank> lists that VBlank's batches whose texture is the image they draw
        // into, with their primitives, whether the texture footprint meets the drawn pixels and whether primitives
        // overlap earlier ones of the batch.
        {
            static const long long selfAt =
                std::getenv("GT4_CORE2_SELF_TRACE") ? std::atoll(std::getenv("GT4_CORE2_SELF_TRACE")) : -1;
            if (selfAt >= 0 && static_cast<long long>(vblanks_) == selfAt && draw.texture != kNoImage &&
                (draw.texture == draw.frame || draw.texture == draw.depth)) {
                std::size_t overlapping = 0;
                for (std::size_t i = 1; i < batch.prims.size(); ++i) {
                    for (std::size_t j = 0; j < i; ++j) {
                        if (!Intersect(batch.prims[i].rect, batch.prims[j].rect).Empty()) {
                            ++overlapping;
                            break;
                        }
                    }
                }
                const PixelRect &f = textureFootprint_;
                std::fprintf(stderr,
                             "[core2] self vblank=%llu %s fb=%u fbw=%u psm=%u rect=%d,%d,%d,%d tex footprint=%d,%d,%d,%d "
                             "meets=%d prims=%zu type=%u overlapping=%zu first=%d,%d,%d,%d\n",
                             static_cast<unsigned long long>(vblanks_), draw.texture == draw.frame ? "frame" : "depth",
                             t.frameView.basePointer, t.frameView.bufferWidth, static_cast<unsigned>(t.frameView.psm),
                             t.rect.x0, t.rect.y0, t.rect.x1, t.rect.y1, f.x0, f.y0, f.x1, f.y1,
                             Intersect(f, t.rect).Empty() ? 0 : 1, batch.prims.size(),
                             batch.prims.empty() ? 0u : static_cast<unsigned>(batch.prims[0].type), overlapping,
                             batch.prims.empty() ? 0 : batch.prims[0].rect.x0, batch.prims.empty() ? 0 : batch.prims[0].rect.y0,
                             batch.prims.empty() ? 0 : batch.prims[0].rect.x1, batch.prims.empty() ? 0 : batch.prims[0].rect.y1);
                if (!batch.prims.empty() && batch.prims[0].count >= 2) {
                    const Vertex &a = batch.vertices[batch.prims[0].first];
                    const Vertex &b = batch.vertices[batch.prims[0].first + 1];
                    std::fprintf(stderr,
                                 "[core2] self   tex1=%llx fst=%u xy4 %d,%d %d,%d uv %u,%u %u,%u stq %.4f,%.4f,%.4f "
                                 "%.4f,%.4f,%.4f\n",
                                 static_cast<unsigned long long>(batch.state.ctx.tex1),
                                 static_cast<unsigned>((batch.state.effectivePrim >> 8) & 1u), a.x4, a.y4, b.x4, b.y4,
                                 a.u, a.v, b.u, b.v, a.s, a.t, a.q, b.s, b.t, b.q);
                }
            }
        }
        // Diagnostics: GT4_CORE2_SKIP=<vblank>:<first>[-<last>] leaves out the drawn batches of that VBlank with those
        // indices (counted from 1 in drawing order, as the batch trace numbers them), to find which draw makes an
        // artifact.
        {
            static long long skipAt = -2, skipFirst = 0, skipLast = 0;
            if (skipAt == -2) {
                skipAt = -1;
                if (const char *text = std::getenv("GT4_CORE2_SKIP")) {
                    long long a = 0, b = 0, c = 0;
                    const int n = std::sscanf(text, "%lld:%lld-%lld", &a, &b, &c);
                    if (n >= 2) {
                        skipAt = a;
                        skipFirst = b;
                        skipLast = n == 3 ? c : b;
                    }
                }
            }
            static long long lastVblank = -1, index = 0;
            if (static_cast<long long>(vblanks_) != lastVblank) {
                lastVblank = static_cast<long long>(vblanks_);
                index = 0;
            }
            ++index;
            if (skipAt >= 0 && static_cast<long long>(vblanks_) == skipAt && index >= skipFirst && index <= skipLast) {
                return;
            }
            // GT4_CORE2_SKIP_TEX=<texture base block> and GT4_CORE2_SKIP_PRIMS=<primitive count>: the same, by the
            // batch's identity, at every VBlank.
            static const long long skipTex =
                std::getenv("GT4_CORE2_SKIP_TEX") ? std::atoll(std::getenv("GT4_CORE2_SKIP_TEX")) : -1;
            static const long long skipPrims =
                std::getenv("GT4_CORE2_SKIP_PRIMS") ? std::atoll(std::getenv("GT4_CORE2_SKIP_PRIMS")) : -1;
            if (skipTex >= 0 && t.hasTexture && static_cast<long long>(tex0::Tbp0(batch.state.ctx.tex0)) == skipTex) return;
            if (skipPrims >= 0 && static_cast<long long>(t.draws.size()) == skipPrims) return;
            // GT4_CORE2_SKIP_TEST=<TEST register, hex>: leaves out batches with exactly that TEST value.
            static const long long skipTest =
                std::getenv("GT4_CORE2_SKIP_TEST") ? std::strtoll(std::getenv("GT4_CORE2_SKIP_TEST"), nullptr, 16) : -1;
            if (skipTest >= 0 && static_cast<long long>(batch.state.ctx.test) == skipTest) return;
            // GT4_CORE2_NEAREST_TEX=<texture base block>: batches with that texture sample it nearest.
            static const long long nearestTex =
                std::getenv("GT4_CORE2_NEAREST_TEX") ? std::atoll(std::getenv("GT4_CORE2_NEAREST_TEX")) : -1;
            if (nearestTex >= 0 && static_cast<long long>(tex0::Tbp0(batch.state.ctx.tex0)) == nearestTex) {
                draw.state.fields[gpu::gpufield::TexLinear] = 0;
            }
            // GT4_CORE2_NOATE_TEX=<texture base block>: the alpha test of batches with that texture always passes.
            static const long long noAteTex =
                std::getenv("GT4_CORE2_NOATE_TEX") ? std::atoll(std::getenv("GT4_CORE2_NOATE_TEX")) : -1;
            if (noAteTex >= 0 && static_cast<long long>(tex0::Tbp0(batch.state.ctx.tex0)) == noAteTex) {
                draw.state.fields[gpu::gpufield::Ate] = 0;
            }
        }
        static const long long traceAt = std::getenv("GT4_CORE2_BATCH_TRACE_AT")
                                             ? std::atoll(std::getenv("GT4_CORE2_BATCH_TRACE_AT"))
                                             : -1;  // diagnostics only
        if (traceAt >= 0 && static_cast<long long>(vblanks_) == traceAt) {
            const std::uint64_t tex0 = batch.state.ctx.tex0;
            std::fprintf(stderr,
                         "[core2] batch frame=%u/%u/%02x z=%u tex=%u/%u/%02x from=%s rect=%d,%d,%d,%d prims=%zu "
                         "fbmsk=%08x prim=%llx test=%llx zbuf=%llx alpha=%llx verts=%zu tex1=%llx tex0=%llx\n",
                         t.frameView.basePointer, t.frameView.bufferWidth, static_cast<unsigned>(t.frameView.psm),
                         depthTarget ? t.zView.basePointer : 0u, t.hasTexture ? tex0::Tbp0(tex0) : 0u,
                         t.hasTexture ? tex0::Tbw(tex0) : 0u, t.hasTexture ? tex0::Psm(tex0) : 0u,
                         !t.hasTexture ? "-" : draw.texture != kNoImage ? "target" : "memory", t.rect.x0, t.rect.y0,
                         t.rect.x1, t.rect.y1, t.draws.size(), t.state.fields[gpu::gpufield::Fbmsk],
                         static_cast<unsigned long long>(batch.state.effectivePrim),
                         static_cast<unsigned long long>(batch.state.ctx.test),
                         static_cast<unsigned long long>(batch.state.ctx.zbuf),
                         static_cast<unsigned long long>(batch.state.ctx.alpha), batch.vertices.size(),
                         static_cast<unsigned long long>(batch.state.ctx.tex1), static_cast<unsigned long long>(tex0));
            static const long long vertsAbove = std::getenv("GT4_CORE2_TRACE_VERTS")
                                                    ? std::atoll(std::getenv("GT4_CORE2_TRACE_VERTS"))
                                                    : -1;  // diagnostics: vertices of batches with this many prims
            if (vertsAbove >= 0 && static_cast<long long>(t.draws.size()) >= vertsAbove) {
                for (const Vertex &v : batch.vertices) {
                    std::fprintf(stderr, "[core2]   v xy4=%d,%d z=%u rgba=%u,%u,%u,%u stq=%.6g,%.6g,%.6g uv=%u,%u\n",
                                 v.x4, v.y4, v.z, v.r, v.g, v.b, v.a, v.s, v.t, v.q, v.u, v.v);
                }
            }
        }
        if (d1Stats_) NoteD1Class(t, draw);
        if (t.hasTexture) {  // C1 survey: draws that ask for mip levels (TEX1.MXL > 0 and MMIN 2..5)
            const std::uint64_t tex1 = batch.state.ctx.tex1;
            const std::uint32_t mxl = static_cast<std::uint32_t>((tex1 >> 2) & 7u);
            const std::uint32_t mmin = static_cast<std::uint32_t>((tex1 >> 6) & 7u);
            if (mxl > 0 && mmin >= 2 && mmin <= 5) {
                stats.mipDraws++;
                if (mmin == 3 || mmin == 5) stats.trilinearDraws++;
                if ((tex1 & 1u) != 0) stats.mipConstantLod++;
                stats.mipMaxLevels = std::max<std::uint64_t>(stats.mipMaxLevels, mxl);
            }
        }
        std::string error;
        bool drawn = false;
        {
            ScopedUs timer(clock_, &PhaseTimes::draw);
            drawn = gpu_.Draw(draw, error);
            stats.specialisedBatches = gpu_.Stats().specialisedBatches;
            stats.computeFills = gpu_.Stats().computeFills;
            stats.selfCopies = gpu_.Stats().selfCopies;
        }
        if (!drawn) {
            Fail(error);
            return;
        }
        counters.gpuDraws += t.draws.size();
        if (t.blended) counters.batchesBlended++;
        const std::uint64_t number = ++drawCounter_;
        frameTarget->valid = Union(frameTarget->valid, t.rect);
        frameTarget->drawn = Union(frameTarget->drawn, t.rect);
        frameTarget->lastDraw = number;
        frameTarget->lastDrawVblank = vblanks_;
        frameTarget->usedThisFrame = true;
        if (t.frameView.psm == Psm::Ct32 && t.state.fields[gpu::gpufield::Fbmsk] != 0xFFFFFFFFu) {
            frameTarget->fullWordWritten = true;
        }
        if (depthTarget) {
            depthTarget->usedThisFrame = true;
            if (t.state.fields[gpu::gpufield::Zmsk] == 0) {
                depthTarget->valid = Union(depthTarget->valid, t.rect);
                depthTarget->drawn = Union(depthTarget->drawn, t.rect);
                depthTarget->lastDraw = number;
                depthTarget->lastDrawVblank = vblanks_;
                if (t.zView.psm == Psm::Z32) depthTarget->fullWordWritten = true;
            }
        }
    }

    // ---- Display (5) -----------------------------------------------------------------------------------------
    // The image a circuit shows: a colour target at its base (5.1), else a 1x image of GS memory (5.2).
    ImageId memoryImage_ = kNoImage;
    std::uint32_t memoryImageWidth_ = 0, memoryImageHeight_ = 0;

    // IN-531: the bases, widths and format classes written as a buffer by something other than a draw (an upload, a store, a move).
    static std::uint64_t BaseWriteKey(std::uint32_t basePointer, std::uint32_t bufferWidth, std::uint32_t psmCode) {
        const auto psm = memory::decodePsm(psmCode);
        return (std::uint64_t{basePointer} << 16) | (std::uint64_t{bufferWidth} << 8) | static_cast<std::uint64_t>(psm ? ClassOf(*psm) : FormatClass::None);
    }
    std::unordered_map<std::uint64_t, std::uint64_t> baseWrites_;  // the write sequence just after the write
    const bool showAnyDisplayMemory_ = std::getenv("GT4_CORE2_DISPLAY_MEMORY") != nullptr &&
                                       std::strcmp(std::getenv("GT4_CORE2_DISPLAY_MEMORY"), "1") == 0;
    struct NoteBaseWrite {
        Impl &self;
        std::uint64_t key;
        ~NoteBaseWrite() { self.baseWrites_[key] = self.writeSequence_; }
    };

    bool CircuitSource(const display::CircuitRect &circuit, ImageId &image, std::uint32_t &sourceScale,
                       std::uint32_t &readX, std::uint32_t &readY) {
        const auto psm = memory::decodePsm(circuit.psm);
        if (!psm) return false;
        const BufferView view{memory::blockOfPage(circuit.fbp), circuit.fbw, *psm};
        const FormatClass cls = ClassOf(*psm);
        Target *found = nullptr;
        for (Target &t : targets_) {
            if (!t.alive || IsDepthClass(t.cls) || t.view.basePointer != view.basePointer) continue;
            if (t.cls != cls) continue;
            if (!found || t.lastDraw > found->lastDraw) found = &t;
        }
        const std::int32_t dbx = circuit.dbx, dby = circuit.dby;
        if (found && dbx >= 0 && dby >= 0) {
            if (!Grow(*found, static_cast<std::uint32_t>(dbx) + circuit.read_width,
                      static_cast<std::uint32_t>(dby) + circuit.read_height) ||
                !ApplyDirty(*found)) {
                return false;
            }
            found->usedThisFrame = true;
            image = found->image;
            sourceScale = scale_;
            readX = static_cast<std::uint32_t>(dbx);
            readY = static_cast<std::uint32_t>(dby);
            stats.displayFromTargets++;
            return true;
        }
        // 5.2: no target holds the buffer (FMVs, loading screens the EE uploaded): GS memory at 1x.
        // IN-531: only when something wrote this buffer as a buffer of this base and width. GT4 turns the display on at the race's
        // return to the menus (480i) some VBlanks before it draws: that memory holds textures of other widths and old depth, which
        // the reference renderer does not show. It reads black, like a circuit with no source. GT4_CORE2_DISPLAY_MEMORY=1 shows it.
        const bool showAnyMemory = showAnyDisplayMemory_;
        // Nothing else wrote the displayed pages since (uploads of other textures over them make it junk again).
        const auto baseWrite = baseWrites_.find(BaseWriteKey(view.basePointer, view.bufferWidth, circuit.psm));
        const PixelRect displayed{dbx, dby, dbx + static_cast<std::int32_t>(circuit.read_width) - 1, dby + static_cast<std::int32_t>(circuit.read_height) - 1};
        const bool writtenAsBuffer = baseWrite != baseWrites_.end() && UnchangedSince(WriteSpan(view, displayed), baseWrite->second);
        static const bool traceMemory = std::getenv("GT4_CORE2_DISPLAY_TRACE") != nullptr;
        if (traceMemory) {
            std::fprintf(stderr, "[core2] display from memory vblank=%llu bp=%u bw=%u psm=%02x written_as_buffer=%d\n",
                         static_cast<unsigned long long>(vblanks_), view.basePointer, view.bufferWidth, static_cast<unsigned>(view.psm), writtenAsBuffer ? 1 : 0);
        }
        if (!writtenAsBuffer && !showAnyMemory) return false;
        const std::uint32_t w = circuit.read_width, h = circuit.read_height;
        if (w == 0 || h == 0 || w > 2048 || h > 2048) return false;
        std::string error;
        if (memoryImage_ == kNoImage || memoryImageWidth_ < w || memoryImageHeight_ < h) {
            if (memoryImage_ != kNoImage) gpu_.DestroyImage(memoryImage_);
            memoryImageWidth_ = std::max(memoryImageWidth_, w);
            memoryImageHeight_ = std::max(memoryImageHeight_, h);
            memoryImage_ = gpu_.CreateImage(memoryImageWidth_, memoryImageHeight_, false, error);
            if (memoryImage_ == kNoImage) {
                Fail(error);
                return false;
            }
        }
        std::vector<std::uint32_t> words(static_cast<std::size_t>(w) * h);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint32_t sx = static_cast<std::uint32_t>(dbx + static_cast<std::int32_t>(x)) & 2047u;
                const std::uint32_t sy = static_cast<std::uint32_t>(dby + static_cast<std::int32_t>(y)) & 2047u;
                words[static_cast<std::size_t>(y) * w + x] = memory_.Local().readPixel(view, sx, sy);
            }
        }
        if (!gpu_.Patch(memoryImage_, 0, 0, static_cast<std::int32_t>(w) - 1, static_cast<std::int32_t>(h) - 1, words,
                        0xFFFFFFFFu, error)) {
            Fail(error);
            return false;
        }
        image = memoryImage_;
        sourceScale = 1;
        readX = 0;
        readY = 0;
        stats.displayFromMemory++;
        return true;
    }

    // 13.4 at scale (IN-405): the old core's history rules around Gpu::Deinterlace.
    bool DeinterlaceGpu(display::DeinterlaceMode mode, int vblankField, std::uint32_t width, std::uint32_t height,
                        std::string &error) {
        DeinterlaceHistory &h = deinterlace_;
        if (h.width != width || h.height != height) {
            h.Clear();
            h.width = width;
            h.height = height;
        }
        const int rowParity = (vblankField ^ (settings_.bottomFieldFirst ? 1 : 0)) & 1;
        gpu::GpuDrawer::DeinterlaceStep step;
        step.width = width;
        step.height = height;
        step.rowParity = static_cast<std::uint32_t>(rowParity);
        step.field = static_cast<std::uint32_t>(vblankField & 1);
        // IN-386: one output row per algorithm row (the reference); GT4_HW_DEINTERLACE_ROWS=native: one native row.
        const bool nativeRows = std::getenv("GT4_HW_DEINTERLACE_ROWS") != nullptr &&
                                       std::strcmp(std::getenv("GT4_HW_DEINTERLACE_ROWS"), "native") == 0;
        step.rowStep = nativeRows ? scale_ : 1u;
        // This field's entry: a free one, else the oldest.
        constexpr std::uint32_t kNone = gpu::kGpuDeinterlaceHistory;
        std::uint32_t current = kNone;
        for (std::uint32_t i = 0; i < kNone && current == kNone; ++i) {
            if (!h.valid[i]) current = i;
        }
        if (current == kNone) {
            current = 0;
            for (std::uint32_t i = 1; i < kNone; ++i) {
                if (h.age[i] < h.age[current]) current = i;
            }
        }
        // The previous same- and opposite-parity fields, and (IN-396) the older opposite-parity field.
        std::uint64_t sameAge = 0, otherAge = 0, olderAge = 0;
        for (std::uint32_t i = 0; i < kNone; ++i) {
            if (i == current || !h.valid[i]) continue;
            if (h.parity[i] == rowParity) {
                if (step.previousSame == kNone || h.age[i] > sameAge) {
                    step.previousSame = i;
                    sameAge = h.age[i];
                }
            } else if (step.previousOther == kNone || h.age[i] > otherAge) {
                step.olderOther = step.previousOther;
                olderAge = otherAge;
                step.previousOther = i;
                otherAge = h.age[i];
            } else if (step.olderOther == kNone || h.age[i] > olderAge) {
                step.olderOther = i;
                olderAge = h.age[i];
            }
        }
        step.current = current;
        step.haveOutput = h.output;
        step.initWoven = !h.woven;
        switch (mode) {
            case display::DeinterlaceMode::Weave: step.mode = 2; break;
            case display::DeinterlaceMode::Blend: step.mode = 3; break;
            case display::DeinterlaceMode::Bob: step.mode = 4; break;
            default: {
                // IN-405: moving pixels interpolated (mode 6); GT4_HW_DEINTERLACE_MOVING=reference keeps rule 5.
                const bool reference = std::getenv("GT4_HW_DEINTERLACE_MOVING") != nullptr &&
                                              std::strcmp(std::getenv("GT4_HW_DEINTERLACE_MOVING"), "reference") == 0;
                step.mode = reference ? 5 : 6;
                break;
            }
        }
        if (!gpu_.Deinterlace(step, error)) return false;
        if (step.mode >= 5) {
            h.valid[current] = true;  // only the adaptive mode keeps a field history
            h.parity[current] = rowParity;
            h.age[current] = ++h.clock;
            h.output = true;
        }
        if (step.mode == 2 || step.mode == 3) h.woven = true;
        counters.deinterlacedFrames++;
        return true;
    }

    bool ComposeGpu(writeback::GpuFrame &out) {
        out = writeback::GpuFrame{};
        if (failed_ || !gpu_.Ready()) return false;
        if (composedVblank_ == vblanks_ && lastFrame_.valid) {
            out = lastFrame_;  // already composed (and deinterlaced) this VBlank
            return true;
        }
        frontEnd_.FlushBatch(FlushReason::VBlank);
        const display::DisplayLayout layout = display::compute_layout(displayRegs_, previousDby_);
        if (!layout.valid) {
            counters.blankFrames++;
            return false;
        }
        std::array<bool, 2> available{};
        for (std::size_t i = 0; i < 2; ++i) {
            const display::CircuitRect &c = layout.circuits[i];
            available[i] = c.enabled && c.read_width != 0 && c.read_height != 0;
        }
        const display::PmodeFields pmode = display::decode_pmode(displayRegs_.pmode);
        const display::MergePlan plan =
            display::plan_merge(pmode, layout.circuits[0], layout.circuits[1], available[0], available[1]);
        // Diagnostics: GT4_CORE2_DISPLAY_TRACE=<first>-<last> prints the display registers and the colour targets per VBlank.
        {
            static long long traceFirst = -2, traceLast = -1;
            if (traceFirst == -2) {
                traceFirst = -1;
                if (const char *text = std::getenv("GT4_CORE2_DISPLAY_TRACE")) std::sscanf(text, "%lld-%lld", &traceFirst, &traceLast);
            }
            if (traceFirst >= 0 && static_cast<long long>(vblanks_) >= traceFirst && static_cast<long long>(vblanks_) <= traceLast) {
                std::fprintf(stderr, "[core2] display vblank=%llu pmode=%llx bg=%02x%02x%02x outcome=%d",
                             static_cast<unsigned long long>(vblanks_), static_cast<unsigned long long>(displayRegs_.pmode),
                             background_[0], background_[1], background_[2], static_cast<int>(plan.outcome));
                for (std::size_t i = 0; i < 2; ++i) {
                    const display::CircuitRect &c = layout.circuits[i];
                    std::fprintf(stderr, " rc%zu[en=%d use=%d fbp=%u fbw=%u psm=%02x db=%d,%d read=%ux%u]", i + 1, c.enabled ? 1 : 0,
                                 (i == 0 ? plan.use_rc1 : plan.use_rc2) ? 1 : 0, c.fbp, c.fbw, c.psm, c.dbx, c.dby, c.read_width, c.read_height);
                }
                std::fprintf(stderr, " interlaced=%d\n", layout.really_interlaced ? 1 : 0);
                for (const Target &t : targets_) {
                    if (!t.alive || IsDepthClass(t.cls)) continue;
                    std::fprintf(stderr, "[core2]   target bp=%u bw=%u psm=%02x size=%ux%u drawn=%d,%d,%d,%d valid=%d,%d,%d,%d last=%llu\n",
                                 t.view.basePointer, t.view.bufferWidth, static_cast<unsigned>(t.view.psm), t.width, t.height, t.drawn.x0,
                                 t.drawn.y0, t.drawn.x1, t.drawn.y1, t.valid.x0, t.valid.y0, t.valid.x1, t.valid.y1,
                                 static_cast<unsigned long long>(t.lastDraw));
                }
            }
        }
        if (plan.outcome == display::MergeOutcome::KeepPrevious) return false;
        // 13.3 and 13.4 as in the old core (IN-381, IN-405): a progressive frame has no field offset and no
        // deinterlace; an interlaced one gets the step 3 offset of one native row in the field's phase and the GPU
        // deinterlace of the composed image.
        const display::DeinterlaceMode effective =
            settings_.deinterlace == display::DeinterlaceMode::Auto
                ? display::resolve_auto_deinterlace(layout, layout.ffmd, displayRegs_.scanmsk_active)
                : settings_.deinterlace;
        const display::OutputSize size =
            display::merge_output_size(layout, effective == display::DeinterlaceMode::Off, 1);
        if (size.width == 0 || size.height == 0) return false;
        std::uint32_t words[28] = {};
        const std::uint32_t outWidth = size.width * scale_;
        const std::uint32_t outHeight = size.height * scale_;
        words[0] = outWidth;
        words[1] = outHeight;
        words[2] = scale_;
        words[3] = std::uint32_t{background_[0]} | (std::uint32_t{background_[1]} << 8) |
                   (std::uint32_t{background_[2]} << 16) | (std::min<std::uint32_t>(pmode.alp, 255u) << 24);
        words[4] = std::min<std::uint32_t>(pmode.alp, 255u);
        words[5] = pmode.mmod ? 1u : 0u;
        ImageId sources[2] = {kNoImage, kNoImage};
        const bool use[2] = {plan.outcome == display::MergeOutcome::Composite && plan.use_rc1,
                             plan.outcome == display::MergeOutcome::Composite && plan.use_rc2};
        const int vblankField = RowFieldOfDisplayField(oddField_);
        const int fieldPhase = vblankField ^ (settings_.bottomFieldFirst ? 1 : 0);
        const bool interlaced = layout.really_interlaced;
        const std::int32_t offsetRows =
            interlaced ? display::interlace_offset_rows(true, layout.ffmd, effective, 1, fieldPhase) : 0;
        const bool deinterlace = interlaced && effective != display::DeinterlaceMode::Off;
        for (std::size_t i = 0; i < 2; ++i) {
            if (!use[i]) continue;
            const display::CircuitRect &c = layout.circuits[i];
            std::uint32_t sourceScale = 1, readX = 0, readY = 0;
            if (!CircuitSource(c, sources[i], sourceScale, readX, readY)) {
                sources[i] = kNoImage;
                continue;
            }
            std::uint32_t *w = &words[6 + 10 * i];
            w[0] = 1u;
            w[1] = c.psm == 0x01u ? 1u : (c.psm == 0x02u || c.psm == 0x0Au) ? 2u : 0u;
            w[2] = sourceScale;
            w[3] = readX;
            w[4] = readY;
            w[5] = c.read_width;
            w[6] = c.read_height;
            w[7] = static_cast<std::uint32_t>(c.screen_x);
            w[8] = static_cast<std::uint32_t>(static_cast<std::int32_t>(c.screen_y) + offsetRows);
            w[9] = c.display_width;
            words[26 + i] = c.display_height;
        }
        std::string error;
        bool composed = gpu_.Compose(words, sources[0], sources[1], outWidth, outHeight, error);
        if (composed && interlaced) {
            if (offsetRows != 0) counters.interlaceOffsetFrames++;
            if (deinterlace) composed = DeinterlaceGpu(effective, vblankField, outWidth, outHeight, error);
        }
        const bool fxaa = composed && fxaa_;
        if (fxaa) composed = gpu_.Fxaa(outWidth, outHeight, error);  // IN-376
        std::uint32_t presentWidth = presentWidth_, presentHeight = presentHeight_;
        if (const char *forced = std::getenv("GT4_CORE2_PRESENT_SIZE")) {
            unsigned w = 0, h = 0;
            if (std::sscanf(forced, "%ux%u", &w, &h) == 2) {
                presentWidth = w;
                presentHeight = h;
            }
        }
        // IN-478: FSR and Lanczos cost 4.7 ms of GPU per frame on the HD 620 (0.5 ms on the RX 9060 XT): the plain blit in
        // performance mode on an integrated GPU.
        const bool presentScaled = composed && presentResample_ && !(performance_ && gpu_.Integrated()) && presentWidth != 0 && presentHeight != 0 &&
                                   (presentWidth != outWidth || presentHeight != outHeight || rcasSharpness_ > 0.0f);
        if (presentScaled) {
            composed = gpu_.PresentScale(fxaa, outWidth, outHeight, presentWidth, presentHeight, rcasSharpness_, error);
        }
        if (!composed || !gpu_.Submit(false, error)) {
            Fail(error);
            return false;
        }
        counters.presentationFrames++;
        counters.gpuComposedFrames++;
        counters.scaledComposedFrames++;
        out.valid = true;
        out.width = presentScaled ? presentWidth : outWidth;
        out.height = presentScaled ? presentHeight : outHeight;
        out.image = presentScaled ? gpu_.PresentImage() : fxaa ? gpu_.FxaaImage() : gpu_.DisplayImage();
        out.fxaa = fxaa;
        out.presentScaled = presentScaled;
        composedVblank_ = vblanks_;
        lastFrame_ = out;
        return true;
    }

    // ---- VBlank ---------------------------------------------------------------------------------------------
    bool ageTargets_ = true;  // TASK-082: off for a VBlank where the game drew nothing on purpose (frame skipping)
    void Age() {
        for (Target &t : targets_) {
            if (!t.alive) continue;
            if (t.usedThisFrame) {
                t.unusedFrames = 0;
            } else if (++t.unusedFrames > kAgeLimitFrames) {
                // 6.2: deleted without readback. Its drawn area is lost to memory, as in the reference; a later
                // host read of it sees what memory held (IN-408: unless the GPU path read it, see KeepGpuReadContent).
                KeepGpuReadContent(t);
                DestroyTarget(t);
                stats.targetsAged++;
                continue;
            }
            t.usedThisFrame = false;
        }
    }
};

// ---- Renderer -----------------------------------------------------------------------------------------------

namespace {

// The [core2] session line (replay summary, game log every 600 VBlanks).
void PrintCore2Stats(const Core2Counters &c, const Core2GpuStats &g, std::uint64_t vblanks,
                     const std::string &barrierReport, const clut::ClutUnit &clut) {
    std::fprintf(stderr,
                 "[core2] vblank=%llu targets created=%llu aged=%llu replaced=%llu grown=%llu textures targets=%llu "
                 "memory=%llu cache_hits=%llu decodes=%llu dirty=%llu dirty_px=%llu readbacks=%llu "
                 "readback_px=%llu tone_runs=%llu tone_skipped=%llu tone_alpha_only=%llu display targets=%llu "
                 "memory=%llu indexed_readbacks=%llu direct_readbacks=%llu clut_readbacks=%llu download_readbacks=%llu batched_readbacks=%llu dirty_merged=%llu forced_bilinear=%llu cpu_sprites=%llu live_targets=%llu target_slots=%llu decodes_first=%llu decodes_stale=%llu decodes_clash=%llu decodes_over=%llu clut_gpu_loads=%llu gpu_palettes=%llu clut_reads=%llu glm_writes=%llu textures_over_targets=%llu palette_rows=%llu palette_row_hits=%llu gpu batches=%llu draws=%llu submits=%llu "
                 "mip_draws=%llu trilinear=%llu mip_lcm=%llu mip_max_mxl=%llu mip_textures=%llu mip_levels=%llu waits=%llu recycled_images=%llu window_decodes=%llu window_mb=%.1f full_barriers=%llu barriers_skipped=%llu fragment_barriers=%llu render_passes=%llu draw_flushes=%llu image_mb=%.1f texture_mb=%.1f self_texture=%llu self_copies=%llu self_copy_mpx=%.1f\n",
                 (unsigned long long)vblanks, (unsigned long long)c.targetsCreated, (unsigned long long)c.targetsAged,
                 (unsigned long long)c.targetsReplaced, (unsigned long long)c.targetsGrown,
                 (unsigned long long)c.texturesFromTargets, (unsigned long long)c.texturesFromMemory,
                 (unsigned long long)c.atlasHits, (unsigned long long)g.atlasUploads,
                 (unsigned long long)c.dirtyPatches, (unsigned long long)c.dirtyPatchPixels,
                 (unsigned long long)c.readbacks, (unsigned long long)c.readbackPixels, (unsigned long long)c.toneRuns,
                 (unsigned long long)c.toneDrawsSkipped, (unsigned long long)c.toneAlphaOnlyUnsupported,
                 (unsigned long long)c.displayFromTargets, (unsigned long long)c.displayFromMemory,
                 (unsigned long long)c.indexedFromTargetReadbacks, (unsigned long long)c.directFromTargetReadbacks,
                 (unsigned long long)c.clutReadbacks, (unsigned long long)c.downloadReadbacks,
                 (unsigned long long)c.batchedReadbacks, (unsigned long long)c.dirtyMerged,
                 (unsigned long long)c.forcedBilinearBatches,
                 (unsigned long long)c.cpuSpriteBatches, (unsigned long long)c.liveTargets,
                 (unsigned long long)c.targetSlots, (unsigned long long)c.decodesFirst,
                 (unsigned long long)c.decodesStale, (unsigned long long)c.decodesKeyClash,
                 (unsigned long long)c.decodesOverTargets, (unsigned long long)c.clutTargetLoads,
                 (unsigned long long)c.gpuPaletteDecodes, (unsigned long long)c.clutReads,
                 (unsigned long long)c.materializations, (unsigned long long)c.texturesOverTargets,
                 (unsigned long long)c.paletteRowsWritten, (unsigned long long)c.paletteRowHits,
                 (unsigned long long)g.batches, (unsigned long long)g.draws, (unsigned long long)g.submissions,
                 (unsigned long long)c.mipDraws, (unsigned long long)c.trilinearDraws, (unsigned long long)c.mipConstantLod, (unsigned long long)c.mipMaxLevels, (unsigned long long)c.mipTextures, (unsigned long long)c.mipLevelsDecoded, (unsigned long long)g.waits, (unsigned long long)g.recycledImages, (unsigned long long)g.windowDecodes, static_cast<double>(g.windowBytes) / (1024.0 * 1024.0), (unsigned long long)g.fullBarriers, (unsigned long long)g.barriersSkipped, (unsigned long long)g.fragmentBarriers, (unsigned long long)g.renderPasses, (unsigned long long)g.drawFlushes, static_cast<double>(g.imageBytes) / (1024.0 * 1024.0), static_cast<double>(c.textureBytes) / (1024.0 * 1024.0), (unsigned long long)g.selfTextureBatches, (unsigned long long)g.selfCopies,
                 static_cast<double>(g.selfCopyPixels) / 1.0e6);
    // Diagnostics: GT4_CORE2_BARRIER_TRACE=1 adds the cumulative full barriers by command and hazard.
    static const bool barrierTrace = std::getenv("GT4_CORE2_BARRIER_TRACE") != nullptr;
    if (barrierTrace) std::fprintf(stderr, "[core2] barriers by reason%s\n", barrierReport.c_str());
    if (c.replacementFiles != 0 || c.replacementDumps != 0) {
        std::fprintf(stderr,
                     "[core2] texture replacement files=%llu keys=%llu requests=%llu loads=%llu failed=%llu hits=%llu "
                     "mb=%.1f dumps=%llu\n",
                     (unsigned long long)c.replacementFiles, (unsigned long long)c.replacementKeys,
                     (unsigned long long)c.replacementRequests, (unsigned long long)c.replacementLoads,
                     (unsigned long long)c.replacementFailed, (unsigned long long)c.replacementHits,
                     static_cast<double>(c.replacementBytes) / (1024.0 * 1024.0), (unsigned long long)c.replacementDumps);
    }
    // IN-437: specialised pipelines (batches with their own variant, with the uber one while it compiled).
    std::fprintf(stderr,
                 "[core2] specialised pipelines=%llu states=%llu batches=%llu uber_batches=%llu compile_ms=%.1f "
                 "compile_max_ms=%.1f failures=%llu rop_batches=%llu delayed_downloads=%llu hidden_flare_tables=%llu compute_fills=%llu "
                 "fill_mpx=%.1f unchanged_uploads=%llu stale_by upload=%llu readback=%llu delayed=%llu move=%llu whole=%llu "
                 "block_hits=%llu clut_memo hits=%llu reads=%llu uploads_replayed=%llu (%.0f MiB) whole_clears=%llu plain_batches=%llu road_sheens_skipped=%llu sheens_early=%llu\n",
                 (unsigned long long)g.specPipelines, (unsigned long long)g.specRequested,
                 (unsigned long long)g.specialisedBatches, (unsigned long long)g.uberBatches,
                 static_cast<double>(g.specCompileNs) / 1.0e6, static_cast<double>(g.specCompileMaxNs) / 1.0e6,
                 (unsigned long long)g.specFailures, (unsigned long long)g.ropBatches, (unsigned long long)c.delayedDownloads,
                 (unsigned long long)c.hiddenFlareTables, (unsigned long long)g.computeFills,
                 static_cast<double>(g.fillPixels) / 1.0e6, (unsigned long long)c.unchangedUploads,
                 (unsigned long long)c.staleBy[0], (unsigned long long)c.staleBy[1], (unsigned long long)c.staleBy[2],
                 (unsigned long long)c.staleBy[3], (unsigned long long)c.staleBy[4], (unsigned long long)c.contentHits,
                 (unsigned long long)clut.DataMemoHits(), (unsigned long long)clut.DataMemoReads(),
                 (unsigned long long)c.uploadsReplayed, static_cast<double>(c.uploadBytesReplayed) / 1048576.0,
                 (unsigned long long)g.wholeClears, (unsigned long long)g.plainBatches, (unsigned long long)g.roadSheensSkipped, (unsigned long long)g.sheensEarly);
}

}  // namespace

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() {
    PrintCore2Stats(impl_->stats, impl_->gpu_.Stats(), impl_->vblanks_, impl_->gpu_.BarrierReport(), impl_->frontEnd_.Clut());
}

void Renderer::SetScale(std::uint32_t scale, float shift) {
    impl_->scale_ = std::max<std::uint32_t>(scale, 1u);
    impl_->shift_ = shift;
    impl_->frontEnd_.SetCullUncovered(impl_->scale_ <= 1);  // IN-403
}

std::uint32_t Renderer::Scale() const { return impl_->scale_; }

bool Renderer::PrepareShaders(const std::function<void(std::size_t, std::size_t)> &progress, std::string &error) {
    return impl_->gpu_.PrepareShaders(progress, error);
}

bool Renderer::Init(const gpu::VulkanHandles &handles, std::string &error) {
    if (!impl_->gpu_.Init(handles, impl_->scale_, impl_->shift_, error)) return false;
    const char *textureMb = std::getenv("GT4_CORE2_TEXTURE_MB");
    if (textureMb != nullptr && std::atoll(textureMb) > 0) {
        impl_->textureBudget_ = static_cast<std::uint64_t>(std::atoll(textureMb)) << 20;
    } else {
        impl_->textureBudget_ = impl_->gpu_.Integrated() ? kTextureCacheBytesIntegrated : kTextureCacheBytes;
    }
    impl_->StartTextureReplacement();
    if (impl_->delayedDownloads_ || impl_->hideFlares_) {
        std::fprintf(stderr, "[core2] performance mode (decision 0073): delayed downloads %s, flares hidden %s\n",
                     impl_->delayedDownloads_ ? "on" : "off", impl_->hideFlares_ ? "on" : "off");
    }
    std::fprintf(stderr, "[core2] texture cache budget %llu MiB (%s GPU)\n",
                 static_cast<unsigned long long>(impl_->textureBudget_ >> 20),
                 impl_->gpu_.Integrated() ? "integrated" : "discrete");
    return true;
}

bool Renderer::GifTransfer(std::uint32_t path, const std::uint8_t *bytes, std::size_t byteCount) {
    if (path == 0 || path > 3 || byteCount % 16 != 0) return false;
    Impl &s = *impl_;
    ScopedUs gifTimer(s.clock_, &PhaseTimes::gif);
    const std::size_t count = byteCount / 16;
    if constexpr (std::endian::native == std::endian::little && sizeof(Qword) == 16) {
        if (reinterpret_cast<std::uintptr_t>(bytes) % alignof(Qword) == 0) {
            s.frontEnd_.GifTransfer(path, reinterpret_cast<const Qword *>(bytes), count);
            return true;
        }
    }
    s.gifScratch_.resize(count);
    std::memcpy(s.gifScratch_.data(), bytes, byteCount);
    s.frontEnd_.GifTransfer(path, s.gifScratch_.data(), count);
    return true;
}

bool Renderer::GifBusy(std::uint32_t path) const { return impl_->frontEnd_.GifBusy(path); }

void Renderer::GifReset(std::uint32_t pathMask) {
    impl_->frontEnd_.GifReset(pathMask);
    impl_->memory_.ResetRead();
    impl_->pendingRead_.reset();
    impl_->zeroRead_ = false;
}

bool Renderer::WritePrivileged(std::uint32_t address, std::uint64_t value) {
    using namespace priv;
    auto &regs = impl_->displayRegs_;
    switch (address) {
        case kPmode: regs.pmode = value; return true;
        case kSmode1: regs.smode1 = value; return true;
        case kSmode2: regs.smode2 = value; return true;
        case kSyncv: regs.syncv = value; return true;
        case kDispfb1: regs.circuits[0].dispfb = value; return true;
        case kDisplay1: regs.circuits[0].display = value; return true;
        case kDispfb2: regs.circuits[1].dispfb = value; return true;
        case kDisplay2: regs.circuits[1].display = value; return true;
        case kBgcolor:
            impl_->background_[0] = static_cast<std::uint8_t>(FieldOf(value, 0, 8));
            impl_->background_[1] = static_cast<std::uint8_t>(FieldOf(value, 8, 8));
            impl_->background_[2] = static_cast<std::uint8_t>(FieldOf(value, 16, 8));
            return true;
        case kCsr:
            if (FieldOf(value, 9, 1) != 0) ResetGs();
            return true;
        default:
            return false;
    }
}

void Renderer::ResetGs() {
    Impl &s = *impl_;
    s.ReadbackAll();  // 4.4: a reset keeps local memory, so every drawn area is stored first
    s.frontEnd_.ResetGs();
    s.frontEnd_.GifReset(7u);
    s.clutSource_.fill(0);  // IN-408: the CPU CLUT unit is authoritative again
    s.gpuClutMirror_ = false;
    s.clutFromTargetPending_ = false;
    s.clutMirrorPending_.fill(false);
    s.clutMirrorAnyPending_ = false;
    s.memory_.ResetRead();
    s.pendingRead_.reset();
    s.delayed_.clear();
    s.zeroRead_ = false;
    s.displayRegs_ = {};
    s.displayRegs_.anti_blur = s.settings_.antiBlur;
    s.previousDby_ = {};
    s.background_ = {};
    s.toneActive_ = false;
}

void Renderer::ClearVram() {
    Impl &s = *impl_;
    for (Target &t : s.targets_) s.DestroyTarget(t);
    s.delayed_.clear();
    auto bytes = s.memory_.Local().bytes();
    std::fill(bytes.begin(), bytes.end(), std::uint8_t{0});
    s.MarkWritten(BlockSpan{0, kAllBlocks});
    s.recentUploads_.clear();
}

std::size_t Renderer::ReadFifo(void *destination, std::size_t byteCount) {
    Impl &s = *impl_;
    if (byteCount == 0) return 0;
    ScopedUs fifoTimer(s.clock_, &PhaseTimes::fifo);
    s.frontEnd_.FlushBatch(FlushReason::Transfer);
    if (s.pendingRead_ && s.memory_.LocalToHostPending()) {
        const LocalToHostSetup &setup = *s.pendingRead_;
        const auto psm = memory::decodePsm(setup.spsm);
        if (psm && setup.rrw != 0 && setup.rrh != 0 && setup.ssax + setup.rrw <= 2048u && setup.ssay + setup.rrh <= 2048u) {
            const BufferView view{setup.sbp, setup.sbw, *psm};
            const PixelRect rect{static_cast<std::int32_t>(setup.ssax), static_cast<std::int32_t>(setup.ssay),
                                 static_cast<std::int32_t>(setup.ssax + setup.rrw - 1u),
                                 static_cast<std::int32_t>(setup.ssay + setup.rrh - 1u)};
            if (s.hideFlares_ && *psm == Psm::Ct32 && setup.rrw == 64u && setup.rrh == 8u) {
                s.zeroRead_ = true;  // IN-441: no readback; the bytes below are replaced by zeros
                s.stats.hiddenFlareTables++;
            } else if (!s.delayedDownloads_ || !s.AnswerDelayedDownload(view, rect)) {
                const std::uint64_t before = s.stats.readbacks;
                s.readbackReason_ = "download";
                s.Readback(SpanOfRect(view, rect), &view, &rect);
                s.readbackReason_ = "other";
                if (s.stats.readbacks != before) s.stats.downloadReadbacks++;
            }
        } else {
            s.ReadbackAll();
            s.stats.downloadReadbacks++;
        }
        s.pendingRead_.reset();
    }
    bool overrun = false;
    const std::vector<std::uint8_t> data = s.memory_.ReadLocalToHost(byteCount, overrun);
    if (overrun) s.counters.fifoOverruns++;
    if (s.zeroRead_) {
        std::memset(destination, 0, data.size());
    } else {
        std::memcpy(destination, data.data(), data.size());
    }
    if (s.memory_.ReadDrained()) {
        s.frontEnd_.WriteRegister(reg::kTrxdir, 3);
        s.zeroRead_ = false;
    }
    return data.size();
}

void Renderer::Flush() {
    Impl &s = *impl_;
    ScopedUs vblankTimer(s.clock_, &PhaseTimes::vblank);
    static const BlockRange kEverything{0, kAllBlocks};
    s.frontEnd_.VBlank(&kEverything, 1);
    s.frontEnd_.FlushBatch(FlushReason::VBlank);
    ++s.vblanks_;
    if (s.displayRegs_.smode2 & 1u) {
        ++s.counters.vblanksInterlaced;
    } else {
        ++s.counters.vblanksProgressive;
    }
    if (s.ageTargets_) {
        s.Age();
    } else {
        // TASK-082: a VBlank with no drawing on purpose does not count as time for a target either (IN-509's grace is in VBlanks: a
        // Z target drawn every second frame went over it, and six readbacks came back with each one).
        for (Target &t : s.targets_) {
            if (t.alive) ++t.lastDrawVblank;
        }
    }
    s.TrimTexturePool();
    s.NoteSpike();
    // S3: GT4_CORE2_GPU_PROFILE_AT=<vblank> maps the GPU cost of the next VBlank's commands.
    if (s.profileAt_ >= 0) {
        std::string error;
        if (static_cast<long long>(s.vblanks_) == s.profileAt_ + 1 && !s.gpu_.ProfileReport(error)) s.Fail(error);
        if (static_cast<long long>(s.vblanks_) == s.profileAt_ && !s.gpu_.ProfileStart(error)) s.Fail(error);
    }
    if (s.vblanks_ % 600 == 0) {
        s.stats.liveTargets = 0;
        for (const Target &t : s.targets_) s.stats.liveTargets += t.alive ? 1u : 0u;
        s.stats.targetSlots = s.targets_.size();
        PrintCore2Stats(s.stats, s.gpu_.Stats(), s.vblanks_, s.gpu_.BarrierReport(), s.frontEnd_.Clut());
        if (s.d1Stats_) s.PrintD1Classes();
        const PhaseTimes &p = s.times_;
        const double f = 600.0 * 1000.0;  // microseconds over 600 VBlanks -> milliseconds per VBlank
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const double wall = std::chrono::duration<double, std::milli>(now - last).count() / 600.0;
        last = now;
        std::fprintf(stderr, "[core2] wall ms/vblank=%.2f (the whole emulator, GS thread included)\n", wall);
        std::fprintf(stderr,
                     "[core2] ms/vblank gif=%.2f batch=%.2f upload=%.2f (%.0f) written=%.2f clut=%.2f (%.0f) hook=%.2f glm_hook=%.2f translate=%.2f targets=%.2f texture=%.2f draw=%.2f readback=%.2f "
                     "patch=%.2f compose=%.2f flush=%.2f cpu_sprites=%.2f sprite_check=%.2f gpu_wait=%.2f glm=%.2f (%.0f calls, "
                     "%.0f pages) swizzle=%.2f (%.0f KiB) gpu_busy=%.2f (%.1f submits) gpu_draw_passes=%.2f prims=%.0f fragments=%.0f compute=%.0f\n",
                     p.gif / f, p.batch / f, p.upload / f, p.uploads / 600.0, p.written / f, p.clut / f, p.clutLoads / 600.0, p.hook / f, p.glmHook / f, p.translate / f, p.targets / f, p.texture / f, p.draw / f, p.readback / f,
                     p.patch / f, p.compose / f, p.flush / f, p.cpuSprites / f, p.spriteCheck / f,
                     static_cast<double>(s.gpu_.Stats().waitNs - s.lastWaitNs_) / 600.0e6, p.glm / f, p.glmCalls / 600.0,
                     p.glmPages / 600.0, p.swizzle / f, p.swizzleBytes / 600.0 / 1024.0,
                     static_cast<double>(s.gpu_.Stats().gpuNs - s.lastGpuNs_) / 600.0e6,
                     static_cast<double>(s.gpu_.Stats().timedSubmissions - s.lastTimedSubmissions_) / 600.0,
                     static_cast<double>(s.gpu_.Stats().drawPassNs - s.lastDrawPassNs_) / 600.0e6,
                     static_cast<double>(s.gpu_.Stats().primitives - s.lastPrimitives_) / 600.0,
                     static_cast<double>(s.gpu_.Stats().fragments - s.lastFragments_) / 600.0,
                     static_cast<double>(s.gpu_.Stats().computeInvocations - s.lastCompute_) / 600.0);
        {
            // TASK-027 CPU survey: exclusive stage times (each minus the stages nested in it); gif_parse is the GIF
            // front end itself, vblank_own the VBlank work outside the stages it runs.
            const PhaseTimes &x = s.clock_.exclusive;
            const double all = x.gif + x.batch + x.upload + x.written + x.clut + x.hook + x.glmHook + x.translate +
                               x.targets + x.texture + x.draw + x.readback + x.patch + x.compose + x.flush +
                               x.cpuSprites + x.spriteCheck + x.glm + x.swizzle + x.vblank + x.fifo;
            std::fprintf(stderr,
                         "[core2] exclusive ms/vblank total=%.2f gif_parse=%.2f batch_own=%.2f translate=%.2f "
                         "targets=%.2f texture=%.2f clut=%.2f draw=%.2f upload_own=%.2f swizzle=%.2f written=%.2f "
                         "readback=%.2f patch=%.2f glm=%.2f glm_hook=%.2f hook=%.2f sprite_check=%.2f cpu_sprites=%.2f "
                         "flush=%.2f compose=%.2f vblank_own=%.2f fifo_own=%.2f\n",
                         all / f, x.gif / f, x.batch / f, x.translate / f, x.targets / f, x.texture / f, x.clut / f,
                         x.draw / f, x.upload / f, x.swizzle / f, x.written / f, x.readback / f, x.patch / f,
                         x.glm / f, x.glmHook / f, x.hook / f, x.spriteCheck / f, x.cpuSprites / f, x.flush / f,
                         x.compose / f, x.vblank / f, x.fifo / f);
            s.clock_.exclusive = PhaseTimes{};
        }
        {
            // TASK-027 CPU survey R9: batches flushed per VBlank by reason (chapter 02.5 rules, CLUT loads, transfers).
            static const char *const kReasonNames[] = {"prim",  "test",   "dimx",      "alpha",    "fog",
                                                       "frame", "zbuf",   "texture",   "clut",     "transfer",
                                                       "vblank", "autoflush", "capacity", "reset", "sync"};
            static_assert(std::size(kReasonNames) == static_cast<std::size_t>(FlushReason::Count));
            std::string reasons;
            for (std::size_t r = 0; r < std::size(kReasonNames); ++r) {
                const std::uint64_t count = s.frontEnd_.FlushReasonCount(static_cast<FlushReason>(r));
                char part[48];
                std::snprintf(part, sizeof part, " %s=%.1f", kReasonNames[r],
                              static_cast<double>(count - s.lastFlushCounts_[r]) / 600.0);
                reasons += part;
                s.lastFlushCounts_[r] = count;
            }
            const std::uint64_t sameTexture = s.frontEnd_.ClutFlushesSameTexture();
            std::fprintf(stderr, "[core2] batch flushes/vblank%s clut_same_texture=%.1f\n", reasons.c_str(),
                         static_cast<double>(sameTexture - s.lastClutSameTexture_) / 600.0);
            s.lastClutSameTexture_ = sameTexture;
        }
        s.lastPrimitives_ = s.gpu_.Stats().primitives;
        s.lastFragments_ = s.gpu_.Stats().fragments;
        s.lastCompute_ = s.gpu_.Stats().computeInvocations;
        s.lastDrawPassNs_ = s.gpu_.Stats().drawPassNs;
        s.lastGpuNs_ = s.gpu_.Stats().gpuNs;
        s.lastTimedSubmissions_ = s.gpu_.Stats().timedSubmissions;
        std::string byReason;
        std::uint64_t attributed = 0;
        for (Impl::ReasonWait &w : s.reasonWaits_) {
            char part[96];
            std::snprintf(part, sizeof part, " %s=%.2f (%.2f)", w.reason, static_cast<double>(w.waitNs) / 600.0e6,
                          static_cast<double>(w.readbacks) / 600.0);
            byReason += part;
            attributed += w.waitNs;
            w.waitNs = 0;
            w.readbacks = 0;
        }
        const std::uint64_t allWaits = s.gpu_.Stats().waitNs - s.lastWaitNs_;
        std::fprintf(stderr, "[core2] gpu_wait ms/vblank by readback reason (readbacks/vblank):%s not_readback=%.2f "
                     "stores_skipped=%.2f/vblank\n",
                     byReason.c_str(), static_cast<double>(allWaits - std::min(allWaits, attributed)) / 600.0e6,
                     static_cast<double>(s.storesSkipped_) / 600.0);
        s.storesSkipped_ = 0;
        s.lastWaitNs_ = s.gpu_.Stats().waitNs;
        s.times_ = PhaseTimes{};
        s.spikeMark_ = s.TakeSpikeMark();  // the phase times restart from 0
        s.spikeLines_ = 0;
    }
    std::string error;
    {
        ScopedUs timer(s.clock_, &PhaseTimes::flush);
        if (s.gpu_.Ready() && !s.gpu_.Submit(false, error)) s.Fail(error);
    }
}

bool Renderer::ComposeFrameGpu(writeback::GpuFrame &out) {
    ScopedUs timer(impl_->clock_, &PhaseTimes::compose);
    return impl_->ComposeGpu(out);
}

bool Renderer::ReadGpuFrame(const writeback::GpuFrame &frame, std::vector<std::uint32_t> &pixels) {
    if (!frame.valid) return false;
    std::string error;
    if (!impl_->gpu_.ReadDisplay(frame.width, frame.height, pixels, error, frame.presentScaled, frame.fxaa)) {
        impl_->Fail(error);
        return false;
    }
    return true;
}

void Renderer::ComposeFrame(writeback::FrameOutput &out) {
    out = writeback::FrameOutput{};
    writeback::GpuFrame frame;
    if (!ComposeFrameGpu(frame)) return;
    if (!ReadGpuFrame(frame, out.pixels)) return;
    out.valid = true;
    out.width = frame.width;
    out.height = frame.height;
}

void Renderer::SetDisplayField(bool odd) { impl_->oddField_ = odd; }
void Renderer::SetAgeTargets(bool age) { impl_->ageTargets_ = age; }
bool Renderer::DisplayField() const { return impl_->oddField_; }

void Renderer::SetOutputSettings(const writeback::OutputSettings &settings) {
    impl_->settings_ = settings;
    impl_->deinterlace_.Clear();
    impl_->displayRegs_.anti_blur = settings.antiBlur;
}

void Renderer::SetFxaa(bool enabled) { impl_->fxaa_ = enabled; }

void Renderer::SetForcedBilinear(std::uint32_t mode) { impl_->forcedBilinear_ = mode; }

void Renderer::SetPresentSize(std::uint32_t width, std::uint32_t height) {
    impl_->presentWidth_ = width;
    impl_->presentHeight_ = height;
}

const std::uint8_t *Renderer::Slm() {
    impl_->frontEnd_.FlushBatch(FlushReason::Transfer);
    impl_->ReadbackAll();
    return impl_->memory_.Local().bytes().data();
}

const writeback::Counters &Renderer::GetCounters() const { return impl_->counters; }
const Core2Counters &Renderer::Core2Stats() const {
    impl_->stats.ropBatches = impl_->gpu_.Stats().ropBatches;
    impl_->stats.roadSheensSkipped = impl_->gpu_.Stats().roadSheensSkipped;
    impl_->stats.sheensEarly = impl_->gpu_.Stats().sheensEarly;
    return impl_->stats;
}
bool Renderer::Failed() const { return impl_->failed_; }
const std::string &Renderer::FailureText() const { return impl_->failure_; }

}  // namespace hwgs::core2
