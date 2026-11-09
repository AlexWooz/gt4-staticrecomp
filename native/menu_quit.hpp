#pragma once

// TASK-212 (slice421): the native Quit Game row.
//
// The row is a real GT4 widget: the `slice421` data patch adds a `hidden::Quit` child to the `arcade` TopRoot widget
// tree (text "Quit Game", the same font and skew as the other rows), and this module appends that parsed widget to the
// main menu's SelectBox in guest RAM after the script built the menu. It is appended last (after Credit) and removed
// from the hidden template list, so the game renders and scrolls it like every other row. No guest code runs for the
// row: while it is focused the host hides Cross from the guest and a Cross press closes the game through the normal
// window path (card flush, audio close, F8 flush).
//
// The addresses below are v2.00 retail vtables (docs/tasks/GT4-MENU-QUIT-SETTINGS-STUDY.md). A target without them
// disables the feature with one log line.

#include <cstddef>
#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gt4::menu_quit {

struct Status {
  bool found = false;       // the main menu SelectBox is present
  bool injected = false;    // the Quit Game row is part of it
  int index = -1;           // focused row
  int rows = 0;             // row count
};

// Scans guest RAM for the main menu's SelectBox and injects the parsed hidden::Quit widget when the data patch
// created it. Read-only until the tree is recognised; idempotent. Safe to call every VBlank.
Status tick(HWND game_window, std::uint8_t *ram, std::size_t size, std::uint16_t buttons);

// The pad the guest may see: Cross and Circle are hidden while the Quit Game row is focused, so the row's missing
// script module is never dispatched.
std::uint16_t guest_buttons(const Status &status, std::uint16_t buttons);

// The `--press` schedule's mask for the current VBlank (headless runs have no window and no controller poll).
std::uint16_t scheduled_buttons();

void reset();

} // namespace gt4::menu_quit
