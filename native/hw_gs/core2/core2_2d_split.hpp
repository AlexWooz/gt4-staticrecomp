// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/COREGS3-2D-SPLIT.md (TASK-090 phase 4), clean room (decision 0070).
//
// The stream-side rule of the 2D overlay split: where in a GS frame the 3D image ends and the 2D layer begins.
//
// The measured night/A-Spec frames draw the 3D content into the scene buffer (FBP 0x210, CT32 640 wide), run the
// full-screen post chain there, and end it with a full-screen present blit into the display buffer (FBP 0 or 0x140,
// CT16 640 wide); every later batch into the display buffer is the 2D/HUD layer (gt4_coregs3_gs --census:
// full-screen-present-blit at stream seq 2007, the HUD sprites after it). The EE's panel pass marker lands after those
// sprites in the captured stream, so the marker cannot be the cut (docs/COREGS3-2D-SPLIT.md §4). The display registers
// cannot name the target either: during the frame DISPFB still points at the buffer being scanned out, and the game
// flips it to the frame's own buffer only at the end of the frame (the flip ordering the VBlank compose relies on).
// This rule therefore cuts by the stream's own content:
//
//   * a full-screen draw into a CT16 target that samples another colour target (unblended) is the present blit: arm,
//     and remember the target and the draw's rectangle (the display buffer's whole area);
//   * the next batch into that target is the first 2D batch: split there;
//   * a later batch into that target that samples the split framebuffer is refused (the overlay target shadows it):
//     the caller draws that batch into the 3D target and counts it;
//   * a frame with no present blit is not split.
//
// This is a pure state machine: no GPU, no Vulkan, no guest state, no display registers.
// native/hw_gs/core2/core2_2d_split_fixture.cpp pins the ordering, the activation, the refusals and the per-VBlank reset.
#pragma once

#include <cstdint>

namespace gt4::coregs3::split {

// One target's identity in the GS memory a batch names: the base block, the buffer width in 64-pixel units and the raw
// PSM code. Two views that compare equal name the same framebuffer.
struct View {
    std::uint32_t baseBlock = 0;
    std::uint32_t bufferWidth = 0;
    std::uint32_t psm = 0;
    bool valid = false;
};

bool SameView(const View &a, const View &b);

// One batch's place in the frame: its FRAME view, the colour target it samples (valid when the batch samples a
// target), and the drawn rectangle in target pixels (inclusive).
struct Batch {
    View frame;
    View texture;
    std::int32_t x0 = 0;
    std::int32_t y0 = 0;
    std::int32_t x1 = -1;
    std::int32_t y1 = -1;
};

// The renderer's classification of a batch as the frame's present blit: a full-screen draw into a CT16 colour target
// that samples another colour target, unblended. `rect` is the draw's rectangle, which becomes the display buffer's
// read rectangle (the present blit covers the whole display area).
struct PresentCandidate {
    View view;
    std::int32_t x0 = 0;
    std::int32_t y0 = 0;
    std::int32_t x1 = -1;
    std::int32_t y1 = -1;
    bool valid = false;
};

enum class Action {
    None,         // not the present blit and not into the split target: the batch belongs to the 3D image
    PresentBlit,  // the present blit: the cut is armed for this target
    Activate,     // the first 2D batch after the present blit: compose the checkpoint and redirect this batch
    Redirect,     // a 2D batch of a frame already split: draw it into the overlay target
    RefuseSample  // a 2D batch that samples the split framebuffer: draw it into the 3D target and count it
};

// What the rule saw, for the [core2] line and the evidence: frames and batches, not pixels.
struct Counters {
    std::uint64_t presentBlits = 0;         // present blits that armed a frame
    std::uint64_t activated = 0;            // frames whose split started at the first 2D batch
    std::uint64_t redirected = 0;           // batches drawn into the overlay target (Activate included)
    std::uint64_t refusedSamples = 0;       // 2D batches that sampled the split framebuffer (not redirected)
    std::uint64_t layoutChanges = 0;        // frames whose split the caller refused (checkpoint/overlay failure)
};

class Rule {
public:
    // Looks at one batch in stream order. `vblank` is the GS worker's VBlank counter; a new value starts a new frame.
    // `present` is the caller's present-blit classification (ignored while the frame is already armed or split).
    Action Observe(std::uint64_t vblank, const Batch &batch, const PresentCandidate &present);
    // The caller refused the frame after Activate (no checkpoint, no overlay target): the rest of the frame is the
    // faithful path and no overlay is delivered. Counted as a layout refusal.
    void Refuse();
    void Reset(std::uint64_t vblank);

    bool active() const { return active_; }
    bool armed() const { return armed_; }
    std::uint64_t vblank() const { return vblank_; }
    // The split target (the display buffer) and its read rectangle, when armed or active.
    const View &view() const { return view_; }
    std::int32_t x0() const { return x0_; }
    std::int32_t y0() const { return y0_; }
    std::int32_t x1() const { return x1_; }
    std::int32_t y1() const { return y1_; }
    const Counters &counters() const { return counters_; }
    // The session totals, swapped out by the caller (the per-frame state stays).
    void TakeCounters(Counters &out);

private:
    std::uint64_t vblank_ = ~0ull;
    bool armed_ = false;
    bool active_ = false;
    View view_{};
    std::int32_t x0_ = 0, y0_ = 0, x1_ = -1, y1_ = -1;
    Counters counters_{};
};

}  // namespace gt4::coregs3::split
