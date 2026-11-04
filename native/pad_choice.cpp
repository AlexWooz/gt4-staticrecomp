#include "pad_mapping.hpp"

// TASK-148: the controller chosen in the launcher (GT4_PAD). Apart from the mapping so the standalone start rules
// (standalone.cpp, which validates game.ini's `pad`) link it without the controls table.

#include <cctype>

namespace gt4::pad {

namespace {
std::string lowercase(std::string text) {
  for (char &character : text)
    if (character >= 'A' && character <= 'Z')
      character = static_cast<char>(character - 'A' + 'a');
  return text;
}
} // namespace

bool parse_pad_choice(const char *value, PadChoice &choice) {
  if (!value || !*value)
    return false;
  const std::string text(value);
  const std::size_t slash = text.find('/');
  const std::string guid = text.substr(0, slash);
  const std::string serial = slash == std::string::npos ? std::string() : text.substr(slash + 1);
  if (guid.size() != 32 || serial.size() > 64)
    return false;
  for (const char character : guid)
    if (!std::isxdigit(static_cast<unsigned char>(character)))
      return false;
  for (const char character : serial)
    if (character < 0x21 || character > 0x7E)
      return false;
  choice.guid = lowercase(guid);
  choice.serial = serial;
  return true;
}

bool choose_pad(const std::vector<PadCandidate> &pads, const PadChoice *choice, std::uint32_t &instance) {
  if (pads.empty())
    return false;
  const PadCandidate *best = nullptr;
  if (choice) {
    // The same unit first, then the first pad of the same model.
    for (const bool same_serial : {true, false}) {
      if (same_serial && choice->serial.empty())
        continue;
      for (const PadCandidate &pad : pads)
        if (lowercase(pad.guid) == choice->guid && (!same_serial || pad.serial == choice->serial) &&
            (!best || pad.instance < best->instance))
          best = &pad;
      if (best)
        break;
    }
  }
  if (!best)
    for (const PadCandidate &pad : pads)
      if (!best || pad.instance < best->instance)
        best = &pad;
  instance = best->instance;
  return true;
}

} // namespace gt4::pad
