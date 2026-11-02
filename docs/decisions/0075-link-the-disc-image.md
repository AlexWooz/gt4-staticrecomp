# 0075: the launcher links the disc image and no longer copies it

Date: 2026-11-02. Owner's decision, asked for by players and needed for Android. Replaces the owner rule of 2026-10-20 ("the image is always copied").

## Decision

Setup checks the image where the player keeps it (the same full SHA-256 and pins as before), extracts what the game needs into `prepared`, and records the image's path in the receipt (`image.linked_path`, receipt schema 3). The game reads the image there, so one image can serve this and an emulator.

- **The image moved or changed** (the cheap check of every start: size and modified time): the game shows as not installed and the install asks for the image again. Choosing it links it again, with no copy and no new download.
- **Nothing in the onboarding's order changes.** The first row checks the image instead of copying it.

## Safety for those who already have the game

A receipt with no `linked_path` is an install from before this decision: it keeps using its copy in `disc`. An update uninstalls nobody and deletes nothing. If such a player chooses the image again, the old copy stays until they remove the game.

Removing a game deletes the launcher's own files only. An image the player linked is never touched, even one inside the old `disc` folder.

## Paths

The game takes the disc path in the ANSI code page, a limit of the frozen donor runtime. Before this the image was always in the launcher's folder, so no one met it.

- Accents (`C:\\Users\\João`) fit the code page on a Western Windows and go as they are (owner).
- A name outside it (Japanese on a Portuguese Windows) goes by its short 8.3 name.
- With neither, setup refuses the image and says why, rather than start a game with no disc.

## Limits

An image outside the launcher's folder is saved by its absolute path, so on another PC it asks to be chosen once. An image inside the folder is saved relative to it (owner, 2026-11-02), so a portable folder carries it along. The `game` folder is refused as a place for the image, because updates replace it. A linked image's time may differ by 2 seconds: a file system can round it when the image is carried there (exFAT, FAT). An emulator holding the same image open at the same time was not tested.
