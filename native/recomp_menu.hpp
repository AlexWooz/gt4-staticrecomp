#pragma once

// TASK-213 Phase 2a: the "Recomp settings" host menu on the game's main menu SelectBox.
//
// The main-menu row pipeline (menu_quit) proves a host row can be appended to the arcade SelectBox. This module appends
// a `Recomp settings` row from a hidden template composite in the same data patch, and swaps the SelectBox's row list to
// host menus built from the game's own row widgets:
//
//   main menu -> Recomp settings -> hub (Graphics, Controls, Display, Audio, System) -> submenu
//
// The rows are the game's `Composite`/`TextFace` widgets (no floating panel); the SelectBox owns the drawing, focus
// and scrolling; this module owns the row list swap, the Cross/Circle actions and the feedback file. It never writes
// settings.toml and never touches the guest save.
//
// prepare() runs before menu_quit::tick (it appends the Recomp row so menu_quit appends Quit after it); tick() runs
// after menu_quit::tick and handles the buttons. The menu_quit watcher is paused while a host menu is open so it cannot
// re-append the Quit row inside the host list.

#include <cstddef>
#include <cstdint>

namespace gt4::recomp_menu {

// Appends the Recomp row to the arcade main menu once; idempotent. No-op outside the arcade project or after the row
// was appended.
void prepare(std::uint8_t *ram, std::size_t size);

// Handles the host menu state (entry, hub, submenus) for one VBlank; reads the pad edges and writes game-changes.toml.
void tick(std::uint8_t *ram, std::size_t size, std::uint16_t buttons);

// At the render functor entry (after the page update): re-apply the active host list fields/index, so the game's
// SelectBox update cannot reset the host focus. No-op in the main menu.
void on_render_frame(std::uint8_t *ram, std::size_t size);

// True while a host menu (hub or a submenu) is open; the menu_quit watcher must pause then.
bool host_menu_active();

// The pad the guest may see: Cross and Circle are hidden while the host owns the focused row.
std::uint16_t guest_buttons(std::uint16_t buttons);

void reset();

} // namespace gt4::recomp_menu
