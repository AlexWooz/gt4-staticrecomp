# 0077: CoreGS 3.0 phase 1 capture decisions (hooks, no K2, scene A is not a car scene)

Date: 2026-11-05. Branch `task/090-coregs3`. Status: implemented and measured; the phase stays open.

## Context

`docs/COREGS3-PLAN.md` §4/§5.2 specifies phase 1: observe-only hooks at K1 (`0x00455A5C`), K2 (`func_0049CEA0`), K3/K4
(`L_0033EBB4`/`L_0033EBF4`) and K5 (`L_0033EE28`/`L_0033EE54`), a bounded frame snapshot, a VU1 tap and a live overlay
into CoreGS 2.0. The first implementation round covered the hooks, the capture and the offline picture.

Three decisions deviate from the plan's letter and are recorded here so the next round does not re-litigate them.

## 1. K2 is not hooked, because the item already carries the indices

`func_0049CEA0(event)` resolves a 9-bit texture index and a 7-bit material index into a record address using two guest
global arrays and two live cursors. The `event` halfword is part of the item table the K1 hook already copies, and the
arrays and cursors are guest globals (`0x0084AF58/5C/60/64`, `0x00624C7C/80`). The capture computes exactly the addresses
`func_0049CEA0` would compute, from the same words, at the same instant. A second hook would add a call site and a
threading question for no information. **Confirmed** by the captures: 169 texture and 39 material records resolve on a
night-race car frame with zero read failures.

## 2. The frame boundary is the VBlank, not the pass bracket

The first version closed a frame at `car_pass_end`. That produced frames spanning several VBlanks whenever the bracket
did not fire (menus, the licence-test bumper camera), and it made the capture's frame ordinal impossible to align with a
bench dump. The capture now closes the frame the guest just finished at the donor's per-VBlank callback and stamps it
with that VBlank; the K5 bracket only decides which draws are *stored*. `L_0033EBB4`/`L_0033EBF4`/`L_0033EE28`/`L_0033EE54`
remain wired for the pass and camera records.

## 3. The plan's P1-A scene (`licenca-b1`) is an identity scene, not a car scene

Measured: during the timed run the K5 bracket runs every frame and captures **0** draws. The reference frame at VBlank
2060 is a bumper camera (road, HUD, back-mirror strip, no car body); the car appears only at the result screen (~VBlank
3000) through a different pass. The plan's P1-B (`night-run.sh`, six cars) captures 116 car draws per frame and is the car
scene this round used. **Owner decision needed**: keep P1-A as the identity scene and accept P1-B/P1-C as the car scenes,
or record a B-1 run with a camera that shows the car.

## Consequences

- Phase 1's G-OFF/G-SHADOW logic gate is met on P1-A (clock, digest and state identical). Frame identity is not an oracle
  at 1x: the reference build differs from itself in frame 3275, so that check must run at 3x.
- The VU1 vertex formula and the triangle topology are open (plan step 1d). The offline pictures are a labelled point
  cloud until the tap lands.
- Nothing ships: CoreGS 3.0 stays off by default, has no launcher entry and changes no pin or release artefact.
