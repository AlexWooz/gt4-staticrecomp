# 0074: a new install starts with CoreGS 2.0 when the PC runs it; GSdx is optional

Date: 2026-11-02. Owner's decision. Limits the "faithful renderer stays the default" rule of the mission for **new installs only**.

## Decision

Setup no longer downloads GSdx as a matter of course. On the first game of a new install, when no renderer was chosen and GSdx is not installed, setup checks the PC:

1. the graphics card a run would use reports Vulkan 1.2 with its own driver;
2. the game program starts CoreGS 2.0 on that card without a window and compiles the shader cache (`--prepare-shaders`, IN-507).

Both pass: CoreGS 2.0 is stored as the renderer and nothing is downloaded. Otherwise GSdx is the renderer and is downloaded, and the setup page says why. GSdx stays one choice away in Settings: when it is missing, a dialog asks before the download and shows it; declining keeps the renderer stored.

## Why step 2

Vulkan 1.2 is not all CoreGS 2.0 needs: the backend refuses a device without its required features and formats. The game program applies the renderer's own rules, so the launcher keeps no second copy of them, and the compile is one the first Play would do anyway.

## What does not change

- A player who chose a renderer keeps it. A player on GSdx keeps it, on update and when setting up a second game.
- `Settings`' default stays GSdx, so an update never moves a player who never chose. The renderer of a new install is stored as chosen.

## Cost

The last setup step takes as long as the compile: 15 seconds on the RX 9060 XT, 3 min 40 s on the HD 620 (i5-7200U), with a count on the page. It has no Cancel, like the GSdx download before it.
