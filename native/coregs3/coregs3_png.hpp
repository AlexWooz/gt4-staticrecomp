// TASK-090 CoreGS 3.0: a dependency-free PNG writer for the offline milestone pictures.
//
// The tool must produce a PNG without adding a library to the game executable (MIT-only dependencies, and no zlib in the
// tree). The writer therefore emits a valid PNG with DEFLATE "stored" blocks: no compression, about 1.2 MB for a
// 640x480 frame, which is irrelevant for a test tool. It is never linked into gt4vulkan.
#pragma once

#include <cstdint>
#include <string>

namespace gt4::coregs3 {

// Writes an 8-bit RGBA image (top-down rows). Returns false on an I/O error.
bool write_png_rgba(const std::string &path, std::uint32_t width, std::uint32_t height,
                    const std::uint8_t *rgba);

}  // namespace gt4::coregs3
