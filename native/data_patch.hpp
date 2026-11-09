#pragma once

// TASK-212 (slice421): same-length game-data substitution at the disc sector reader.
//
// The game reads GT4.VOL through ps2::disc::read. A patch container built by scripts/gt4_data_patch.py carries
// (absolute ISO byte offset, length, original SHA-256, replacement bytes, replacement SHA-256) records whose length
// equals the file's stored extent, so the volume TOC, the file sizes and every other file stay untouched. This module
// loads the container named by GT4_DATA_PATCH after the disc is open, verifies every original range against the image
// before activating, and rewrites the overlapping bytes of each read.
//
// It is off unless GT4_DATA_PATCH names a valid container. A missing file, a malformed container, a hash mismatch or a
// range outside the image deactivates the whole patch and logs one line; the game then runs unmodified. No game data is
// stored in the executable: the container is derived from the user's own disc at run time.

#include <cstddef>
#include <cstdint>

namespace gt4::data_patch {

// Reads GT4_DATA_PATCH and verifies every record against the image the runtime already opened. Idempotent; returns
// whether at least one range is active.
bool install_after_disc_open();

bool active();

// Called by the derived disc.cpp after the reader produced `size` bytes for the image starting at `offset`.
void apply(std::uint64_t offset, std::uint8_t *bytes, std::size_t size);

// Test seams: install one verified range from memory (offset, original and replacement of `size` bytes), and reset.
bool install_for_test(std::uint64_t offset, const std::uint8_t *original, const std::uint8_t *replacement,
                      std::size_t size);
void reset_for_test();

} // namespace gt4::data_patch
