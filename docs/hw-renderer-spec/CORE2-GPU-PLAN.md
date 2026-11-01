# CoreGS 2.0: GPU plan for 720p on the HD 620 (2026-10-31)

Owner's goal: GT4 at 720p and 60 FPS on an i5-7200U with an HD 620 (15 W shared by CPU and GPU), and less GPU and power on
every machine. Reference: GSdx on D3D11 peaks around 30% GPU at 1x on that laptop, CoreGS 2.0 around 50%; GSdx reached about 44
FPS at 2x in the Arcade night race (approximate), so parity alone may not reach 60 at night.

**Rule for every cut (owner):** equal or imperceptible picture goes to the normal mode; a cut or lowered effect goes to
Performance Mode; anything that changes gameplay (what the EE reads back) is never cut.

## Where the time goes (HD 620, one isolated VBlank, candidate build T5)

| Scene | 1x | 2x | Largest at 2x |
|---|---|---|---|
| Day, Time Trial | 9.7 ms | 25.0 ms | opaque 3D 8.65; depth-writing untextured passes 4.3; full-screen blends about 5 |
| Night, Arcade with cars | 12.7 ms | 31.4 ms | opaque 3D 8.2-9.6; full-screen and from-target blends about 8; depth-writing passes about 4 |

RX 9060 XT, night, 12x: 19.2 ms isolated (21 busy): opaque 3D 3.3, fills 4.0, blends about 5. It barely changes from 1x to 2x (2.7
ms), so fill problems show on it only at 12x. Colour attachments on the 9060 XT at 12x: worse (GPU 19.3 to 21.9 ms): integrated GPUs
only. The isolated map overstates small commands; each step is judged by GPU busy in a normal run.

## Why GSdx costs less (oracle report, prose, ranked by judgement)

Real depth with early rejection; one hardware texture fetch against eight loads; targets the driver can compress; clears; blends
that need no interlock. Draw merging and state sorting explain little. Details: journal of 2026-10-31.

## Steps, cheapest first

Each step: one switch for A/B, in a build of its own. It keeps the frame gates unless the step says otherwise.

| Step | What | Class | Estimated gain (not measured) |
|---|---|---|---|
| G0 | Fill benchmark on the HD 620: attachment-only image against storage plus mutable, varying colour | none | decides G6 |
| G1 | Clears: revisit IN-454 with a kept top byte tracked per target; coalesce strips, cancel a clear overwritten before use | normal, exact | medium |
| G2 | Alpha and blend ranges per batch: drop alpha tests that always pass, simplify blends with a known factor | normal, exact | small to medium |
| G3 | Scale per axis and fractional scales: 2 x 1.6 is 1280 x 717, the real 720p, 20% fewer pixels | normal (owner's scale list) | large |
| G4 | Non-overlapping blends without interlock (read a copy once, write through the attachment) | normal, exact | medium |
| G5 | Real depth for opaque 3D at scale above 1x: floored rasteriser depth, early rejection | normal at scale; 1x keeps the exact path | large, largest risk |
| G6 | Targets without storage use, compressible, once G4 and G5 leave some targets unread by shaders | normal, exact | possibly large, depends on G0 |
| G7 | Exact additive blends: source term in the shader, the attachment only adds | normal, exact | small to medium |
| P1 | Effect passes at 1x under a scaled picture | Performance Mode | medium |
| P2 | Blend through attachments when the factor range allows (IN-482 corrected) | Performance Mode | small |
| P3 | Palette expansion with the hardware sampler; depth without floor | Performance Mode or normal, by picture test | medium |
| F1 | Frame skipping, after the boundary with TASK-134 is agreed (game code) | gameplay untouched | for night tracks |

**Kill rule:** under 3% of GPU busy on both machines at the step's own scale, and no sharper picture or lower power: dropped and
recorded in the ledger. G5 is dropped if it gains under 15%.

## Tests of each step

1. **Frames:** 3x night 10/10, Arcade 14/14 and 12x night 31/31 identical, and the track selection 28/28; a step that changes the
   picture at scale names what it relaxes and gets a visual check.
2. **9060 XT:** night race at 12x, GPU busy and wall, both orders, no parallel builds.
3. **HD 620 over SSH, headless, shaders warm:** `rodar.ps1` sets 15 and 16, GPU busy, GS thread and the map, 1x and 2x, day and
   night.
4. **HD 620 with a window (owner's .bat, at milestones only):** frame histogram and clock.
5. **Picture at scale (G3, G5, P1-P3):** dumps at day, night, rain, rally, replay, photo and split screen against the current build.
6. CTest `hw_`, with and without interlock.

## Targets

1x: GPU busy at most about 5 ms by day (now about 8). 2x at 720p: at most about 14 ms by day (now 25-27), about 18 at night (now
30-31), with frame skipping covering the rest at night. These are goals, not promises.

## Results (2026-10-31, overnight, headless)

Laptop numbers without a window, candidate build against the start of the night. The bench's isolated map overstated most steps.

| Step | Result | State |
|---|---|---|
| G0 | Attachment-only image helps only constant colour; with varying colour no difference | G6 dropped |
| G1 | IN-494, IN-495: -6% each on the HD 620 at 2x by day | in, normal |
| G2 | Not done: alpha tests that always pass cost little now that a failed one leaves early (IN-498) | open, small |
| G3 | Not done: 48 places assume an integer scale, among them the reductions and patches; too large to run unattended | open, next |
| G4 | IN-496 instead: no interlock for large disjoint batches, -2.5 to -3.4% | in, integrated only |
| G5 | Probe with a real depth buffer for opaque batches: -3.6% real (isolated: 4.3 to 2.7 ms) | dropped under the 15% rule |
| New | IN-492 depth fail and IN-498 alpha fail before the interlock: -7% and more | in, normal |
| New | Texels through the sampler: -34% in the bench, -1% in the game | dropped |
| New | Colour through attachments with depth in the shader: -4% in the bench | not built |
| P1 | IN-497 small effect targets at 1x: -8% at 2x at night | in, Performance Mode |
| New | IN-499 CT16 frames through attachments: -0.8% | in, normal |
| New | IN-501, IN-502 the road's sheen and the alpha fills that serve it: -4.7% and -5.3% by day | in, Performance Mode |
| New | IN-503 the sheen leaves early where destination alpha is zero: -3.2% by day | in, normal, integrated only |
| Next | Readbacks before a write: 3.0-3.6 ms of waiting per VBlank at 6x on George V Paris, the largest single wait (owner's F8, 2026-11-01); whether some can be delayed or avoided is not studied | to study |
| Option | IN-505 Performance Mode without the colour filter: -4.4% by day, changes the race's colours | measured, owner's decision |
| New | IN-504 the tone palette in push constants: -43% for that pass in the bench, nothing in the game; input attachments: no gain | dropped |

**Candidate 367 against 362, RX 9060 XT 12x night:** GPU busy 20.7 to 19.1 ms (-8%, 2+2 runs).

**Candidate 367 against 362, HD 620 at 2x, no window, shaders warm, GPU busy / wall ms per VBlank:**

| Scene and mode | 362 | 367 | GPU |
|---|---|---|---|
| Day, normal | 25.84 / 28.2 | 18.85 / 20.7 | -27% |
| Day, Performance Mode | 25.74 / 26.5 | 16.75 / 17.5 | -35% |
| Night, normal | 32.9 / 39.5 | 25.4 / 32.5 | -23% |
| Night, Performance Mode | 33.0 / 35.8 | 22.7 / 25.5 | -31% |

These replace the earlier overnight figures: the bench's set 19 turns Performance Mode on, so its "normal" before this table was Performance Mode without the switch being compared. Only `AB_NAME=GT4_CORE2_PERFORMANCE` compares the two modes, and its first warm-up run is still Performance Mode.

What the fill bench says about the HD 620, for later: one store 0.52 ms per pass, two stores (frame and depth) 1.19, a load and a store of one image 0.53, the same under the interlock 1.10, a colour attachment 0.21 to 0.31.
