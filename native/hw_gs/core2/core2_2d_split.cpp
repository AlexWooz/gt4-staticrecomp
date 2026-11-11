// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/COREGS3-2D-SPLIT.md (TASK-090 phase 4), clean room (decision 0070).

#include "core2_2d_split.hpp"

namespace gt4::coregs3::split {

bool SameView(const View &a, const View &b) {
    return a.valid && b.valid && a.baseBlock == b.baseBlock && a.bufferWidth == b.bufferWidth && a.psm == b.psm;
}

void Rule::Reset(std::uint64_t vblank) {
    vblank_ = vblank;
    armed_ = false;
    active_ = false;
    view_ = View{};
    x0_ = y0_ = 0;
    x1_ = y1_ = -1;
}

void Rule::Refuse() {
    active_ = false;
    armed_ = false;
    view_ = View{};
    x0_ = y0_ = 0;
    x1_ = y1_ = -1;
    counters_.layoutChanges += 1;
}

void Rule::TakeCounters(Counters &out) {
    out = counters_;
    counters_ = Counters{};
}

Action Rule::Observe(std::uint64_t vblank, const Batch &batch, const PresentCandidate &present) {
    if (vblank != vblank_) Reset(vblank);
    if (active_) {
        if (!SameView(batch.frame, view_)) return Action::None;  // 3D content or another buffer
        if (batch.texture.valid && batch.texture.baseBlock == view_.baseBlock && batch.texture.psm == view_.psm) {
            // The batch samples the framebuffer the split redirected: the overlay target shadows it. Draw it into the
            // 3D target (still readable) and count it; never sample the overlay as if it were the scene.
            counters_.refusedSamples += 1;
            return Action::RefuseSample;
        }
        counters_.redirected += 1;
        return Action::Redirect;
    }
    if (armed_) {
        // The next batch into the present target is the first 2D batch.
        if (!SameView(batch.frame, view_)) return Action::None;
        active_ = true;
        counters_.activated += 1;
        counters_.redirected += 1;
        return Action::Activate;
    }
    if (present.valid) {
        armed_ = true;
        view_ = present.view;
        x0_ = present.x0;
        y0_ = present.y0;
        x1_ = present.x1;
        y1_ = present.y1;
        counters_.presentBlits += 1;
        return Action::PresentBlit;
    }
    return Action::None;
}

}  // namespace gt4::coregs3::split
