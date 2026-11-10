// TASK-090 CoreGS 3.0: the offline car-texture tool.
//
//   gt4_coregs3_texture <capture.gt4scene> --vram <capture.gt4vram> --frame N --out <folder> [--draw N ...]
//                        [--reference <TextureDumps folder>] [--psnr] [--report <file>]
//       Decodes the textures of the selected draws (all draws when none is named) from the sidecar, writes one PNG per
//       unique texture record, and prints the record state, the decode status and the Format G content key. With
//       --reference it compares every decoded image with the CoreGS 2.0 GT4_HW_DUMP_TEXTURES PNGs by exact RGBA
//       equality, (and, with --psnr, by PSNR against same-size candidates when no exact match exists).
//
//   gt4_coregs3_texture <capture.gt4scene> --vram <capture.gt4vram> --stability --draw N [--report <file>]
//       Decodes draw N in every captured frame and compares each texture record's key (and its RGBA) between the
//       frames: the key-stability control across VBlanks 7398/7399.
//
// Sidecar alignment: the GS worker's record `vblank` is the VSYNC that ended the interval, so the capture frame with
// `frame.vblank == record.vblank - 1` is the frame whose draws the record covers. --vram-vblank selects a record by its
// own number for diagnostics.
//
// A capture is derived from the user's own disc and is never committed: the input, sidecar, PNGs and reports live under
// build/evidence/.
#include "coregs3_png.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_texture.hpp"
#include "coregs3_vram.hpp"

#include "hw_gs_replace/images.hpp"
#include "hw_gs_replace/xxh3.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace gt4::coregs3;

bool read_file(const char *path, std::vector<std::uint8_t> &data) {
  std::FILE *file = std::fopen(path, "rb");
  if (file == nullptr) return false;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(file);
    return false;
  }
  data.resize(static_cast<std::size_t>(size));
  const bool ok = data.empty() || std::fread(data.data(), 1, data.size(), file) == data.size();
  std::fclose(file);
  return ok;
}

bool load_capture(const char *path, SceneHeader &header, std::vector<FrameSnapshot> &frames, std::string &error) {
  std::vector<std::uint8_t> data;
  if (!read_file(path, data)) {
    error = "cannot open " + std::string(path);
    return false;
  }
  std::size_t at = 0;
  if (!parse_scene_header(data.data(), data.size(), header, error)) return false;
  at = 16;
  while (at < data.size()) {
    if (data.size() - at < 8) {
      error = "trailing bytes at the end of the capture";
      return false;
    }
    FrameSnapshot frame;
    const std::size_t consumed = parse_frame_block(data.data() + at, data.size() - at, header.version, frame, error);
    if (consumed == 0) return false;
    frames.push_back(std::move(frame));
    at += consumed;
  }
  return true;
}

const char *psm_name(std::uint32_t psm) {
  switch (psm) {
    case 0x00: return "CT32";
    case 0x13: return "T8";
    case 0x14: return "T4";
    default: return "other";
  }
}

struct ReferenceImage {
  std::string name;
  std::uint64_t payload_hash = 0;  // the Format P texture hash the dump name starts with
  bool indexed = false;            // the name's bits field says an indexed format
  std::uint64_t clut_hash = 0;     // the Format P CLUT hash for an indexed texture
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t hash = 0;  // xxh3 of the decoded RGBA bytes
  std::vector<std::uint8_t> rgba;
};

// The Format P dump name is `<textureHash>[-<clutHash>][-r<W>x<H>]-<bits>.png` (names.hpp). Only the fields the
// comparison needs are parsed: the payload hash, and the CLUT hash of an indexed texture (the bits word names the PSM).
void parse_reference_name(const std::string &stem, ReferenceImage &image) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  for (;;) {
    const std::size_t dash = stem.find('-', start);
    fields.push_back(stem.substr(start, dash == std::string::npos ? std::string::npos : dash - start));
    if (dash == std::string::npos) break;
    start = dash + 1;
  }
  if (fields.size() < 2) return;
  const auto hex_value = [](const std::string &text, std::uint64_t &value) {
    if (text.empty() || text.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    try {
      value = std::stoull(text, nullptr, 16);
      return true;
    } catch (const std::exception &) {
      return false;
    }
  };
  hex_value(fields.front(), image.payload_hash);
  std::uint64_t bits = 0;
  if (!hex_value(fields.back(), bits)) return;
  const std::uint32_t psm = static_cast<std::uint32_t>(bits & 0x3Fu);
  image.indexed = psm == 0x13 || psm == 0x14 || psm == 0x1B || psm == 0x24 || psm == 0x2C;
  if (image.indexed && fields.size() >= 3) hex_value(fields[1], image.clut_hash);
}

struct ReferenceIndex {
  std::vector<ReferenceImage> images;
  std::size_t failed = 0;

  bool load(const std::string &folder, std::string &error) {
    std::error_code code;
    if (!std::filesystem::is_directory(folder, code)) {
      error = "reference folder " + folder + " does not exist";
      return false;
    }
    for (const auto &entry : std::filesystem::directory_iterator(folder, code)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".png") continue;
      std::vector<std::uint8_t> bytes;
      if (!read_file(entry.path().string().c_str(), bytes)) {
        failed += 1;
        continue;
      }
      hw_gs_replace::ReplacementImage image;
      std::string reason;
      if (!hw_gs_replace::DecodePngImage(bytes, image, reason) || image.levels.empty()) {
        failed += 1;
        continue;
      }
      ReferenceImage reference;
      reference.name = entry.path().filename().string();
      reference.width = image.width;
      reference.height = image.height;
      reference.rgba = std::move(image.levels.front().bytes);
      reference.hash = hw_gs_replace::Xxh3Hash64(reference.rgba);
      // FormatPDumpName starts with the Format P texture hash in lowercase hex (names.hpp).
      parse_reference_name(entry.path().stem().string(), reference);
      images.push_back(std::move(reference));
    }
    return true;
  }

  const ReferenceImage *exact(std::uint64_t hash, const std::vector<std::uint8_t> &rgba) const {
    for (const ReferenceImage &image : images)
      if (image.hash == hash && image.rgba.size() == rgba.size() && image.rgba == rgba) return &image;
    return nullptr;
  }
};

double psnr(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) {
  if (a.size() != b.size() || a.empty()) return -1.0;
  double squared = 0.0;
  for (std::size_t index = 0; index < a.size(); ++index) {
    const double difference = static_cast<double>(a[index]) - static_cast<double>(b[index]);
    squared += difference * difference;
  }
  const double mse = squared / static_cast<double>(a.size());
  if (mse == 0.0) return 999.0;
  return 10.0 * std::log10(255.0 * 255.0 / mse);
}

struct DecodeOne {
  std::uint32_t draw = 0;
  std::uint32_t item = 0;
  std::uint32_t record = 0;
  std::uint32_t address = 0;
  DecodedTexture texture;
  std::string png;
};

// Decodes every unique texture record a draw uses. A record shared by several items is decoded once. `draw_sequence` is
// CapturedDraw::sequence, the tool's draw number.
std::vector<DecodeOne> decode_draw(const FrameSnapshot &frame, const SlmImage &slm, std::uint32_t draw_sequence,
                                   const std::string &out_folder, bool write_png) {
  std::vector<DecodeOne> decoded;
  const CapturedDraw *draw = nullptr;
  for (const CapturedDraw &candidate : frame.draws)
    if (candidate.sequence == draw_sequence) {
      draw = &candidate;
      break;
    }
  if (draw == nullptr) return decoded;
  std::vector<bool> seen(frame.textures.size(), false);
  for (std::uint32_t item = 0; item < draw->item_count; ++item) {
    const CapturedItem &captured_item = draw->items[item];
    if (captured_item.texture_record == kNoRecord || captured_item.texture_record >= frame.textures.size()) continue;
    if (seen[captured_item.texture_record]) continue;
    seen[captured_item.texture_record] = true;
    const CapturedTexture &record = frame.textures[captured_item.texture_record];
    DecodeOne one;
    one.draw = draw->sequence;
    one.item = item;
    one.record = captured_item.texture_record;
    one.address = record.address;
    std::string error;
    decode_texture(slm, record.bytes, sizeof record.bytes, one.texture, error);
    if (write_png) {
      char name[128];
      std::snprintf(name, sizeof name, "v%llu_d%u_tex%u_%08x_%s_%ux%u.png",
                    static_cast<unsigned long long>(frame.vblank), static_cast<unsigned>(draw->sequence),
                    static_cast<unsigned>(captured_item.texture_record), static_cast<unsigned>(record.address),
                    psm_name(one.texture.record.psm), static_cast<unsigned>(one.texture.width),
                    static_cast<unsigned>(one.texture.height));
      one.png = (std::filesystem::path(out_folder) / name).string();
      if (one.texture.status == TextureStatus::ok)
        write_png_rgba(one.png, one.texture.width, one.texture.height, one.texture.rgba.data());
    }
    decoded.push_back(std::move(one));
  }
  return decoded;
}

const VramRecord *pick_record(const std::vector<VramRecord> &records, const FrameSnapshot &frame, long long explicit_vblank,
                              std::string &error) {
  const std::uint64_t wanted = explicit_vblank >= 0 ? static_cast<std::uint64_t>(explicit_vblank) : frame.vblank + 1u;
  for (const VramRecord &record : records)
    if (record.vblank == wanted) return &record;
  std::string available;
  for (const VramRecord &record : records) {
    if (!available.empty()) available += ",";
    available += std::to_string(record.vblank);
  }
  error = "no VRAM record at VBlank " + std::to_string(wanted) + " (available " + available + ")";
  return nullptr;
}

void print_texture(std::FILE *output, const DecodeOne &one) {
  const TextureRecord &record = one.texture.record;
  std::fprintf(output,
               "  draw %u item %u record %u address 0x%08X %s(0x%02X) tw %u th %u tbp 0x%04X tbw %u cbp 0x%04X csa %u "
               "cld %u mxl %u wms/wmt %u/%u\n",
               static_cast<unsigned>(one.draw), static_cast<unsigned>(one.item), static_cast<unsigned>(one.record),
               static_cast<unsigned>(one.address), psm_name(record.psm), static_cast<unsigned>(record.psm),
               static_cast<unsigned>(record.tw), static_cast<unsigned>(record.th),
               static_cast<unsigned>(record.tbp0), static_cast<unsigned>(record.tbw),
               static_cast<unsigned>(record.cbp), static_cast<unsigned>(record.csa),
               static_cast<unsigned>(record.cld), static_cast<unsigned>(record.mxl), static_cast<unsigned>(record.wms),
               static_cast<unsigned>(record.wmt));
  if (one.texture.status != TextureStatus::ok) {
    std::fprintf(output, "    status %s: %s\n", texture_status_name(one.texture.status), one.texture.detail.c_str());
    return;
  }
  std::fprintf(output,
               "    status ok %ux%u palette %u key payload %016llx palette %016llx format_p %s png %s\n",
               static_cast<unsigned>(one.texture.width), static_cast<unsigned>(one.texture.height),
               static_cast<unsigned>(one.texture.palette_entries),
               static_cast<unsigned long long>(one.texture.key.textureHash),
               static_cast<unsigned long long>(one.texture.key.paletteHash), one.texture.format_p_name.c_str(),
               one.png.c_str());
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: gt4_coregs3_texture <capture.gt4scene> --vram <capture.gt4vram> --frame N --out <folder>\n"
                 "                             [--draw N ...] [--reference <folder>] [--psnr] [--report <file>]\n"
                 "       gt4_coregs3_texture <capture.gt4scene> --vram <capture.gt4vram> --stability --draw N\n"
                 "                             [--report <file>]\n");
    return 2;
  }
  SceneHeader header;
  std::vector<FrameSnapshot> frames;
  std::string error;
  if (!load_capture(argv[1], header, frames, error)) {
    std::fprintf(stderr, "gt4_coregs3_texture: %s\n", error.c_str());
    return 1;
  }

  const char *vram_path = nullptr;
  const char *out_folder = nullptr;
  const char *report_path = nullptr;
  const char *reference_folder = nullptr;
  long long frame_index = -1;
  long long explicit_vblank = -1;
  bool stability = false;
  bool psnr_enabled = false;
  std::vector<std::uint32_t> draws;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--vram" && index + 1 < argc) vram_path = argv[++index];
    else if (argument == "--out" && index + 1 < argc) out_folder = argv[++index];
    else if (argument == "--report" && index + 1 < argc) report_path = argv[++index];
    else if (argument == "--reference" && index + 1 < argc) reference_folder = argv[++index];
    else if (argument == "--frame" && index + 1 < argc) frame_index = std::atoll(argv[++index]);
    else if (argument == "--vram-vblank" && index + 1 < argc) explicit_vblank = std::atoll(argv[++index]);
    else if (argument == "--draw" && index + 1 < argc) draws.push_back(static_cast<std::uint32_t>(std::atoi(argv[++index])));
    else if (argument == "--stability") stability = true;
    else if (argument == "--psnr") psnr_enabled = true;
    else {
      std::fprintf(stderr, "gt4_coregs3_texture: unknown argument %s\n", argument.c_str());
      return 2;
    }
  }
  if (vram_path == nullptr) {
    std::fprintf(stderr, "gt4_coregs3_texture: --vram is required\n");
    return 2;
  }
  if (!stability && (frame_index < 0 || static_cast<std::size_t>(frame_index) >= frames.size())) {
    std::fprintf(stderr, "gt4_coregs3_texture: --frame N is required and must be one of the %zu frames\n", frames.size());
    return 2;
  }
  if (!stability && out_folder == nullptr) {
    std::fprintf(stderr, "gt4_coregs3_texture: --out is required\n");
    return 2;
  }

  std::vector<VramRecord> records;
  if (!read_vram_sidecar(vram_path, records, error)) {
    std::fprintf(stderr, "gt4_coregs3_texture: %s\n", error.c_str());
    return 1;
  }
  std::FILE *output = stdout;
  if (report_path != nullptr) {
    output = std::fopen(report_path, "wb");
    if (output == nullptr) {
      std::fprintf(stderr, "gt4_coregs3_texture: cannot open %s\n", report_path);
      return 1;
    }
  }
  std::fprintf(output, "capture %s version %u target %u frames %zu, %zu VRAM records from %s\n", argv[1], header.version,
               header.target, frames.size(), records.size(), vram_path);

  int result = 0;
  std::map<std::uint32_t, ReferenceIndex> references;

  if (stability) {
    if (draws.empty()) {
      std::fprintf(stderr, "gt4_coregs3_texture: --stability needs --draw N\n");
      if (output != stdout) std::fclose(output);
      return 2;
    }
    // draw -> record address -> the decode in each frame that holds it.
    std::map<std::uint32_t, std::map<std::uint32_t, std::vector<DecodeOne>>> per_frame;
    for (std::uint32_t draw : draws) {
      for (const FrameSnapshot &frame : frames) {
        const VramRecord *record = pick_record(records, frame, explicit_vblank, error);
        if (record == nullptr) {
          std::fprintf(output, "stability draw %u: %s\n", static_cast<unsigned>(draw), error.c_str());
          result = 1;
          continue;
        }
        SlmImage slm;
        if (!build_slm_image(*record, slm, error)) {
          std::fprintf(output, "stability draw %u: %s\n", static_cast<unsigned>(draw), error.c_str());
          result = 1;
          continue;
        }
        std::fprintf(output, "stability draw %u frame %u vblank %llu record vblank %llu\n", static_cast<unsigned>(draw),
                     frame.index, static_cast<unsigned long long>(frame.vblank),
                     static_cast<unsigned long long>(record->vblank));
        for (const DecodeOne &one : decode_draw(frame, slm, draw, "", false)) {
          print_texture(output, one);
          per_frame[draw][one.address].push_back(one);
        }
      }
    }
    for (std::uint32_t draw : draws) {
      for (const auto &[address, list] : per_frame[draw]) {
        bool equal = list.size() >= 2;
        bool status_ok = true;
        for (std::size_t index = 0; index < list.size(); ++index) {
          status_ok = status_ok && list[index].texture.status == TextureStatus::ok;
          if (index == 0) continue;
          const DecodedTexture &a = list[index - 1].texture;
          const DecodedTexture &b = list[index].texture;
          if (a.status != b.status || a.key.textureHash != b.key.textureHash || a.key.paletteHash != b.key.paletteHash ||
              a.key.width != b.key.width || a.key.height != b.key.height || a.rgba != b.rgba)
            equal = false;
        }
        std::fprintf(output,
                     "stability draw %u address 0x%08X frames %zu status %s key %s (%016llx/%016llx) rgba %s\n",
                     static_cast<unsigned>(draw), static_cast<unsigned>(address), list.size(),
                     status_ok ? "ok" : "refused", equal ? "equal" : "DIFFERS",
                     list.empty() ? 0ull : static_cast<unsigned long long>(list.back().texture.key.textureHash),
                     list.empty() ? 0ull : static_cast<unsigned long long>(list.back().texture.key.paletteHash),
                     equal ? "equal" : "DIFFERS");
        if (!equal || !status_ok) result = 1;
      }
    }
  } else {
    const FrameSnapshot &frame = frames[static_cast<std::size_t>(frame_index)];
    const VramRecord *record = pick_record(records, frame, explicit_vblank, error);
    if (record == nullptr) {
      std::fprintf(output, "frame %u vblank %llu: %s\n", frame.index, static_cast<unsigned long long>(frame.vblank),
                   error.c_str());
      if (output != stdout) std::fclose(output);
      return 1;
    }
    SlmImage slm;
    if (!build_slm_image(*record, slm, error)) {
      std::fprintf(output, "frame %u: %s\n", frame.index, error.c_str());
      if (output != stdout) std::fclose(output);
      return 1;
    }
    std::fprintf(output, "frame %u vblank %llu draws %u textures %u, VRAM record vblank %llu blocks 0x%04X+%u\n",
                 frame.index, static_cast<unsigned long long>(frame.vblank), frame.draw_count, frame.texture_count,
                 static_cast<unsigned long long>(record->vblank), static_cast<unsigned>(record->first_block),
                 static_cast<unsigned>(record->block_count));
    if (reference_folder != nullptr) {
      ReferenceIndex &index = references[0];
      if (!index.load(reference_folder, error)) {
        std::fprintf(output, "reference: %s\n", error.c_str());
        result = 1;
      } else {
        std::fprintf(output, "reference %s: %zu PNGs, %zu refused by the decoder\n", reference_folder, index.images.size(),
                     index.failed);
      }
    }
    std::vector<std::uint32_t> selected = draws;
    if (selected.empty()) {
      for (const CapturedDraw &draw : frame.draws) selected.push_back(draw.sequence);
    }
    std::map<std::uint32_t, const CapturedDraw *> by_sequence;
    for (const CapturedDraw &draw : frame.draws) by_sequence[draw.sequence] = &draw;
    for (std::uint32_t draw : selected) {
      auto found = by_sequence.find(draw);
      if (found == by_sequence.end()) {
        std::fprintf(output, "draw %u: not in this frame\n", static_cast<unsigned>(draw));
        continue;
      }
      const std::vector<DecodeOne> decoded = decode_draw(frame, slm, draw, out_folder, true);
      std::fprintf(output, "draw %u: %zu unique textures\n", static_cast<unsigned>(draw), decoded.size());
      for (const DecodeOne &one : decoded) {
        print_texture(output, one);
        if (one.texture.status != TextureStatus::ok || reference_folder == nullptr) continue;
        ReferenceIndex &index = references[0];
        const ReferenceImage *match = index.exact(hw_gs_replace::Xxh3Hash64(one.texture.rgba), one.texture.rgba);
        if (match != nullptr) {
          std::fprintf(output, "    reference exact %s\n", match->name.c_str());
          continue;
        }
        // The same texture is still identifiable by the Format P payload hash in the dump name; when only the palette
        // changed between the draw and the sidecar's VBlank, the payload hash matches and the CLUT hash does not.
        const ReferenceImage *same_payload = nullptr;
        double same_payload_psnr = -1.0;
        for (const ReferenceImage &image : index.images) {
          if (image.payload_hash != one.texture.format_p.fullChain.textureHash) continue;
          if (image.width != one.texture.width || image.height != one.texture.height) continue;
          const double value = psnr(one.texture.rgba, image.rgba);
          if (same_payload == nullptr || value > same_payload_psnr) {
            same_payload = &image;
            same_payload_psnr = value;
          }
        }
        if (same_payload != nullptr) {
          const bool palette_equal =
              same_payload->indexed && same_payload->clut_hash == one.texture.format_p.fullChain.clutHash;
          std::fprintf(output, "    reference same-payload %s psnr %.2f dB palette %s\n",
                       same_payload->name.c_str(), same_payload_psnr, palette_equal ? "equal" : "DIFFERS");
          continue;
        }
        std::fprintf(output, "    reference no exact match among %zu images\n", index.images.size());
        if (psnr_enabled) {
          double best = -1.0;
          const ReferenceImage *best_image = nullptr;
          std::size_t examined = 0;
          for (const ReferenceImage &image : index.images) {
            if (image.width != one.texture.width || image.height != one.texture.height) continue;
            if (++examined > 16) break;
            const double value = psnr(one.texture.rgba, image.rgba);
            if (value > best) {
              best = value;
              best_image = &image;
            }
          }
          if (best_image != nullptr)
            std::fprintf(output, "    reference best %s psnr %.2f dB (first %zu same-size images)\n",
                         best_image->name.c_str(), best, examined);
          else
            std::fprintf(output, "    reference best: no same-size image\n");
        }
      }
    }
  }

  if (output != stdout) std::fclose(output);
  return result;
}
