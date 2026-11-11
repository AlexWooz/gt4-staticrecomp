// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/COREGS3-2D-SPLIT.md (TASK-090 phase 4), clean room (decision 0070).
//
// CTest native_coregs3_2d_split: the stream-side rule of the 2D overlay split. The measured night/A-Spec frame order
// (3D into the scene buffer, the post chain, a full-screen present blit into the display buffer, then the HUD sprites)
// pins the arm, the activation at the first 2D batch, the redirects, the framebuffer-sample refusal, the other-buffer
// passes, the caller's refusal and the per-VBlank reset. No GPU, no guest state, no display registers.

#include <cstdint>
#include <cstdio>

#include "core2/core2_2d_split.hpp"

namespace {

using gt4::coregs3::split::Action;
using gt4::coregs3::split::Batch;
using gt4::coregs3::split::PresentCandidate;
using gt4::coregs3::split::Rule;
using gt4::coregs3::split::View;

int failures = 0;
int checks = 0;

void Check(bool condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

void CheckAction(Action actual, Action expected, const char *what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL: %s (action %d, expected %d)\n", what, static_cast<int>(actual), static_cast<int>(expected));
    }
}

// The measured display target of the A-Spec night race: FBP 0 (or 0x140), 640 wide, CT16.
View DisplayView(std::uint32_t baseBlock = 0) { return View{baseBlock, 10, 2, true}; }

View SceneView() { return View{6720, 2, 0, true}; }  // the CT32 scene buffers the traced frame draws into
View OtherBufferView() { return View{0x6000, 10, 0, true}; }

Batch FullScreenInto(const View &frame, const View &texture) {
    Batch batch;
    batch.frame = frame;
    batch.texture = texture;
    batch.x0 = 0;
    batch.y0 = 0;
    batch.x1 = 639;
    batch.y1 = 447;
    return batch;
}

Batch SpriteInto(const View &frame, const View &texture, std::int32_t x0, std::int32_t y0) {
    Batch batch;
    batch.frame = frame;
    batch.texture = texture;
    batch.x0 = x0;
    batch.y0 = y0;
    batch.x1 = x0 + 63;
    batch.y1 = y0 + 63;
    return batch;
}

PresentCandidate Present(const View &view) {
    PresentCandidate candidate;
    candidate.view = view;
    candidate.x0 = 0;
    candidate.y0 = 0;
    candidate.x1 = 639;
    candidate.y1 = 447;
    candidate.valid = true;
    return candidate;
}

// The measured frame: scene draws, the post chain, the present blit and the 2D sprites.
void MeasuredFrameOrder() {
    Rule rule;
    const View scene = SceneView();
    const View display = DisplayView();
    CheckAction(rule.Observe(7600, SpriteInto(scene, View{}, 0, 0), PresentCandidate{}), Action::None,
                "a scene draw is 3D");
    CheckAction(rule.Observe(7600, FullScreenInto(scene, scene), PresentCandidate{}), Action::None,
                "a post-chain draw sampling the scene buffer is 3D");
    CheckAction(rule.Observe(7600, FullScreenInto(display, scene), Present(display)), Action::PresentBlit,
                "the full-screen conversion into the display buffer arms the cut");
    Check(rule.armed() && !rule.active(), "armed but not split before the first 2D batch");
    CheckAction(rule.Observe(7600, SpriteInto(display, View{}, 39, 45), PresentCandidate{}), Action::Activate,
                "the first batch into the display buffer after the present blit starts the split");
    Check(rule.active(), "the split is active after the first 2D batch");
    CheckAction(rule.Observe(7600, SpriteInto(display, View{}, 89, 48), PresentCandidate{}), Action::Redirect,
                "a later HUD sprite is redirected");
    CheckAction(rule.Observe(7600, SpriteInto(display, View{}, 300, 300), PresentCandidate{}), Action::Redirect,
                "an unblended HUD sprite is redirected too");
    const auto &counters = rule.counters();
    Check(counters.presentBlits == 1 && counters.activated == 1 && counters.redirected == 3,
          "counters: one present blit, one activation, three redirected batches");
    Check(rule.view().baseBlock == display.baseBlock && rule.x1() == 639, "the split keeps the present target's rect");
}

// A post-chain pass that follows the present blit into the same scene buffer is not a present blit; draw it faithfully.
void OtherBufferPassDoesNotSplit() {
    Rule rule;
    const View scene = SceneView();
    const View display = DisplayView(0x140);
    CheckAction(rule.Observe(7601, FullScreenInto(display, SceneView()), Present(display)), Action::PresentBlit,
                "the present blit arms");
    CheckAction(rule.Observe(7601, FullScreenInto(scene, SceneView()), PresentCandidate{}), Action::None,
                "a full-screen pass into the scene buffer after the present blit stays 3D");
    Check(rule.armed(), "still armed");
    CheckAction(rule.Observe(7601, SpriteInto(display, View{}, 0, 0), PresentCandidate{}), Action::Activate,
                "the first batch into the display buffer splits");
}

// A full-screen draw into the display buffer that samples no target (a 2D clear or a full-screen banner) is 2D.
void FullScreenTwoDSplits() {
    Rule rule;
    const View display = DisplayView();
    CheckAction(rule.Observe(7602, FullScreenInto(display, SceneView()), Present(display)), Action::PresentBlit, "arm");
    CheckAction(rule.Observe(7602, FullScreenInto(display, View{}), PresentCandidate{}), Action::Activate,
                "a full-screen untextured draw after the present blit is the first 2D batch");
}

// A 2D batch that samples the split framebuffer must not read the overlay target.
void FramebufferSampleIsRefused() {
    Rule rule;
    const View display = DisplayView();
    CheckAction(rule.Observe(7603, FullScreenInto(display, SceneView()), Present(display)), Action::PresentBlit, "arm");
    CheckAction(rule.Observe(7603, SpriteInto(display, View{}, 10, 10), PresentCandidate{}), Action::Activate,
                "split");
    CheckAction(rule.Observe(7603, SpriteInto(display, display, 100, 100), PresentCandidate{}), Action::RefuseSample,
                "sampling the split framebuffer is refused");
    CheckAction(rule.Observe(7603, SpriteInto(display, OtherBufferView(), 100, 100), PresentCandidate{}),
                Action::Redirect, "sampling another target is still redirected");
    Check(rule.counters().refusedSamples == 1, "one refused sample counted");
}

// No present blit: a frame that never writes a CT16 present target is never split.
void NoPresentBlitDoesNotSplit() {
    Rule rule;
    const View scene = SceneView();
    CheckAction(rule.Observe(7604, SpriteInto(scene, View{}, 10, 10), PresentCandidate{}), Action::None,
                "a scene draw with no present blit is not split");
    CheckAction(rule.Observe(7604, SpriteInto(scene, View{}, 20, 20), PresentCandidate{}), Action::None,
                "and neither is the next one");
    Check(!rule.active() && rule.counters().presentBlits == 0, "no present blit counted");
}

// A new VBlank starts a new frame: the armed/active state never leaks.
void VblankResetsTheRule() {
    Rule rule;
    const View display = DisplayView();
    CheckAction(rule.Observe(7606, FullScreenInto(display, SceneView()), Present(display)), Action::PresentBlit, "arm");
    CheckAction(rule.Observe(7606, SpriteInto(display, View{}, 0, 0), PresentCandidate{}), Action::Activate, "split");
    CheckAction(rule.Observe(7607, SpriteInto(display, View{}, 0, 0), PresentCandidate{}), Action::None,
                "the next frame's first display draw is unarmed again");
    Check(!rule.active() && !rule.armed(), "the next frame starts clean");
}

// The caller's refusal after Activate (no checkpoint, no overlay target) keeps the frame faithful.
void RefuseKeepsTheFaithfulPath() {
    Rule rule;
    const View display = DisplayView();
    CheckAction(rule.Observe(7608, FullScreenInto(display, SceneView()), Present(display)), Action::PresentBlit, "arm");
    CheckAction(rule.Observe(7608, SpriteInto(display, View{}, 0, 0), PresentCandidate{}), Action::Activate, "split");
    rule.Refuse();
    Check(!rule.active(), "Refuse deactivates the split");
    CheckAction(rule.Observe(7608, SpriteInto(display, View{}, 10, 10), PresentCandidate{}), Action::None,
                "the rest of the frame is drawn normally");
}

// A view with another PSM or width is another framebuffer, not the split target.
void OtherViewIsNotTheSplitTarget() {
    Rule rule;
    const View display = DisplayView();
    CheckAction(rule.Observe(7609, FullScreenInto(display, SceneView()), Present(display)), Action::PresentBlit, "arm");
    Batch wider = SpriteInto(View{display.baseBlock, 20, 2, true}, View{}, 0, 0);
    CheckAction(rule.Observe(7609, wider, PresentCandidate{}), Action::None, "a different width is another framebuffer");
    Batch ct32 = SpriteInto(View{display.baseBlock, 10, 0, true}, View{}, 0, 0);
    CheckAction(rule.Observe(7609, ct32, PresentCandidate{}), Action::None, "a different format is another framebuffer");
    Check(rule.armed(), "still armed for the display buffer");
}

void TakeCountersResetsTheSessionTotals() {
    Rule rule;
    const View display = DisplayView();
    rule.Observe(7611, FullScreenInto(display, SceneView()), Present(display));
    rule.Observe(7611, SpriteInto(display, View{}, 0, 0), PresentCandidate{});
    gt4::coregs3::split::Counters taken;
    rule.TakeCounters(taken);
    Check(taken.presentBlits == 1 && taken.activated == 1, "the totals carry the session's counts");
    Check(rule.counters().presentBlits == 0 && rule.counters().activated == 0, "and the rule starts at zero again");
}

}  // namespace

int main() {
    MeasuredFrameOrder();
    OtherBufferPassDoesNotSplit();
    FullScreenTwoDSplits();
    FramebufferSampleIsRefused();
    NoPresentBlitDoesNotSplit();
    VblankResetsTheRule();
    RefuseKeepsTheFaithfulPath();
    OtherViewIsNotTheSplitTarget();
    TakeCountersResetsTheSessionTotals();
    if (failures != 0) {
        std::printf("coregs3 2D split: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("coregs3 2D split: %d checks passed\n", checks);
    return 0;
}
