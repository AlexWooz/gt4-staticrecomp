// TASK-090 CoreGS 3.0, phase S2: the `.gt4gs` decoder and census. See coregs3_gsdecode.hpp for the rule.
#include "coregs3_gsdecode.hpp"

#include "coregs3_gsstream.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace gt4::coregs3 {
namespace {

std::uint64_t read_u64(const std::uint8_t *p) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(p[index]) << (8 * index);
  return value;
}

std::uint32_t bits(std::uint64_t word, unsigned low, unsigned high) {
  const unsigned width = high - low + 1;
  const std::uint64_t mask = width >= 64 ? ~0ull : ((1ull << width) - 1ull);
  return static_cast<std::uint32_t>((word >> low) & mask);
}

float as_float(std::uint32_t value) {
  float out = 0.0f;
  std::memcpy(&out, &value, sizeof out);
  return out;
}

// The measured 12.4 wrap rule: dx = (x - ofx) & 0xFFFF, sx = dx / 16, then the signed 12-bit range.
float screen_coordinate(std::uint32_t raw, std::uint32_t offset) {
  const std::uint32_t delta = (raw - offset) & 0xFFFFu;
  float value = static_cast<float>(delta) / 16.0f;
  if (value > 2047.9375f) value -= 4096.0f;
  return value;
}

struct Tag {
  std::uint32_t nloop = 0;
  bool eop = false;
  bool pre = false;
  std::uint32_t prim = 0;
  std::uint32_t format = 0;  // 0 packed, 1 reglist, 2 image, 3 disabled
  std::uint32_t nreg = 0;
  std::array<std::uint8_t, 16> regs{};
};

Tag decode_tag(std::uint64_t lo, std::uint64_t hi) {
  Tag tag;
  tag.nloop = bits(lo, 0, 14);
  tag.eop = bits(lo, 15, 15) != 0;
  tag.pre = bits(lo, 46, 46) != 0;
  tag.prim = bits(lo, 47, 57);
  tag.format = std::min<std::uint32_t>(bits(lo, 58, 59), 3u);
  tag.nreg = bits(lo, 60, 63);
  if (tag.nreg == 0) tag.nreg = 16;
  for (std::uint32_t index = 0; index < 16; ++index)
    tag.regs[index] = static_cast<std::uint8_t>((hi >> (4u * index)) & 0xFu);
  return tag;
}

// One pending packet on a path. `remaining` counts qwords for PACKED/IMAGE and 64-bit words for REGLIST.
struct Pending {
  bool active = false;
  Tag tag;
  std::uint32_t remaining = 0;
  std::uint32_t descriptor = 0;
  std::uint64_t start_vblank = 0;
  std::uint64_t start_job = 0;  // the last VU1 job posted when the tag was accepted (the merge order of S3)
  std::uint32_t start_sequence = 0;
  std::uint32_t start_record = 0;
  std::uint32_t start_frame = 0;
  std::uint8_t path = 0;
  std::uint64_t tag_begin_offset = 0;
  std::uint64_t image_bytes = 0;
  std::vector<std::uint8_t> image_data;  // the host->local payload the capture delivered (S3 texture source)
  // Per-packet content flags.
  bool wrote_register = false;
  bool kicked_vertices = false;
  bool image_counted = false;
};

struct PathWalker {
  GsDecodeState state;
  Pending pending;
  bool prim_known = false;
  bool uv_known = false;
  bool uv_from_st = false;
  float last_u = 0.0f, last_v = 0.0f;
  std::uint32_t q_latch_bits = 0;  // 1.0f after a tag resets it; kept for the ST/RGBA latch
  // The batch run currently being collected.
  bool batch_open = false;
  GsDecodeBatch batch;
  std::vector<GsDecodeVertex> run_vertices;
  GsDecodeState run_state;
  bool run_state_known = false;
  // The transfer registers latched by A+D writes for the next IMAGE tag.
  std::uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0, trxdir = 0;
  bool transfer_state_known = false;
};

struct CensusWriter {
  GsStreamCensus &census;
  std::vector<GsDecodeBatch> &batches;
  std::vector<GsDecodeImageUpload> &uploads;
  std::array<PathWalker, 4> paths;  // indexed by GIF path 1..3

  GsFrameCensus &frame(std::uint64_t vblank, std::uint32_t frame_index) {
    (void)frame_index;
    GsFrameCensus &row = census.frames[vblank];
    if (row.capture_vblank == 0) {
      row.capture_vblank = vblank;
      row.scene_frame = vblank > 0 ? vblank - 1 : 0;
    }
    return row;
  }
};

void finish_run(CensusWriter &writer, PathWalker &walker) {
  if (!walker.batch_open || walker.run_vertices.empty()) {
    walker.run_vertices.clear();
    walker.batch_open = false;
    return;
  }
  GsDecodeBatch batch = walker.batch;
  batch.verts = walker.run_vertices;
  batch.vertices = static_cast<std::uint32_t>(batch.verts.size());
  batch.drawing_vertices = 0;
  float min_x = std::numeric_limits<float>::infinity(), max_x = -min_x;
  float min_y = min_x, max_y = -min_x;
  for (const GsDecodeVertex &vertex : batch.verts) {
    if (vertex.drawing) batch.drawing_vertices += 1;
    min_x = std::min(min_x, vertex.x);
    max_x = std::max(max_x, vertex.x);
    min_y = std::min(min_y, vertex.y);
    max_y = std::max(max_y, vertex.y);
  }
  batch.min_x = min_x;
  batch.max_x = max_x;
  batch.min_y = min_y;
  batch.max_y = max_y;
  batch.full_screen = (max_x - min_x) >= 600.0f && (max_y - min_y) >= 400.0f;
  writer.census.batches += 1;
  writer.census.vertices += batch.vertices;
  writer.census.drawing_vertices += batch.drawing_vertices;
  if (batch.drawing_vertices != 0) writer.census.draws += 1;
  if (batch.full_screen) writer.census.full_screen_batches += 1;
  GsFrameCensus &row = writer.frame(batch.vblank, 0);
  row.batches += 1;
  row.vertices += batch.vertices;
  row.drawing_vertices += batch.drawing_vertices;
  if (batch.drawing_vertices != 0) row.draws += 1;
  if (batch.full_screen) row.full_screen_batches += 1;
  writer.batches.push_back(std::move(batch));
  walker.run_vertices.clear();
  walker.batch_open = false;
}

void begin_batch(PathWalker &walker, const GsDecodeState &state) {
  const Pending &pending = walker.pending;
  walker.batch = GsDecodeBatch{};
  walker.batch.vblank = pending.start_vblank;
  walker.batch.sequence = pending.start_sequence;
  walker.batch.last_job = pending.start_job;
  walker.batch.path = pending.path;
  walker.batch.state = state;
  walker.batch_open = true;
  walker.run_vertices.clear();
  walker.run_state_known = false;
}

void add_vertex(PathWalker &walker, std::uint32_t raw_x, std::uint32_t raw_y, std::uint32_t z, std::uint32_t fog,
                bool has_fog, bool drawing) {
  if (!walker.batch_open) begin_batch(walker, walker.state);
  if (!walker.run_state_known) {
    walker.batch.state = walker.state;
    walker.run_state_known = true;
  }
  GsDecodeVertex vertex;
  vertex.x = screen_coordinate(raw_x, walker.state.xyoffset_ofx());
  vertex.y = screen_coordinate(raw_y, walker.state.xyoffset_ofy());
  vertex.z = z;
  vertex.fog = static_cast<std::uint8_t>(fog);
  vertex.has_fog = has_fog;
  vertex.drawing = drawing;
  vertex.rgba = static_cast<std::uint32_t>(walker.state.rgba & 0xFFFFFFFFu);
  if (walker.uv_known) {
    vertex.u = walker.last_u;
    vertex.v = walker.last_v;
    vertex.has_uv = true;
  }
  walker.run_vertices.push_back(vertex);
}

void write_register(CensusWriter &writer, PathWalker &walker, std::uint32_t address, std::uint64_t data) {
  GsDecodeState &state = walker.state;
  writer.census.register_writes += 1;
  writer.census.ad_address_hist[address & 0x7F] += 1;
  switch (address & 0x7F) {
  case 0x00: {  // PRIM
    const std::uint32_t prim = static_cast<std::uint32_t>(data & 0x7FFu);
    if (walker.batch_open && !walker.run_vertices.empty()) finish_run(writer, walker);
    state.prim = prim;
    walker.prim_known = true;
    writer.census.prim_kicks += 1;
    GsFrameCensus &row = writer.frame(walker.pending.active ? walker.pending.start_vblank : 0, 0);
    row.prim_kicks += 1;
    break;
  }
  case 0x01: state.rgba = data; break;
  case 0x02: {
    state.st = data;
    walker.last_u = as_float(static_cast<std::uint32_t>(data & 0xFFFFFFFFu));
    walker.last_v = as_float(static_cast<std::uint32_t>((data >> 32) & 0xFFFFFFFFu));
    walker.uv_known = true;
    walker.uv_from_st = true;
    break;
  }
  case 0x03: {
    state.uv = data;
    walker.last_u = static_cast<float>(data & 0xFFFFu) / 16.0f;
    walker.last_v = static_cast<float>((data >> 16) & 0xFFFFu) / 16.0f;
    walker.uv_known = true;
    walker.uv_from_st = false;
    break;
  }
  case 0x06:
  case 0x07: state.tex0 = data; break;
  case 0x08:
  case 0x09: state.clamp = data; break;
  case 0x0A: state.fog = data; break;
  case 0x14:
  case 0x15: state.tex1 = data; break;
  case 0x16:
  case 0x17: state.tex2 = data; break;
  case 0x18:
  case 0x19: state.xyoffset = data; break;
  case 0x3D: state.fogcol = data; break;
  case 0x3F: break;  // TEXFLUSH
  case 0x40:
  case 0x41: state.scissor = data; break;
  case 0x42:
  case 0x43: state.alpha = data; break;
  case 0x44: state.dimx = data; break;
  case 0x45: state.dthe = data; break;
  case 0x46: state.colclamp = data; break;
  case 0x47:
  case 0x48: state.test = data; break;
  case 0x49: state.pabe = data; break;
  case 0x4C:
  case 0x4D: state.frame = data; break;
  case 0x4E:
  case 0x4F: state.zbuf = data; break;
  case 0x50: walker.bitbltbuf = data; break;
  case 0x51: walker.trxpos = data; walker.transfer_state_known = true; break;
  case 0x52: walker.trxreg = data; walker.transfer_state_known = true; break;
  case 0x53: walker.trxdir = data; walker.transfer_state_known = true; break;
  case 0x60: writer.census.signals += 1; break;  // SIGNAL
  case 0x61: break;  // FINISH
  case 0x62: break;  // LABEL
  default: break;
  }
}

void consume_packed(CensusWriter &writer, PathWalker &walker, std::uint32_t descriptor, std::uint64_t lo,
                    std::uint64_t hi) {
  Pending &pending = walker.pending;
  switch (descriptor) {
  case 0x0: {
    const std::uint32_t prim = static_cast<std::uint32_t>(lo & 0x7FFu);
    if (walker.batch_open && !walker.run_vertices.empty()) finish_run(writer, walker);
    walker.state.prim = prim;
    walker.prim_known = true;
    writer.census.prim_kicks += 1;
    writer.frame(pending.start_vblank, 0).prim_kicks += 1;
    pending.wrote_register = true;
    break;
  }
  case 0x1: {
    // PACKED RGBA: one byte per 32-bit lane, with the Q latch in bits 32-63.
    const std::uint64_t r = bits(lo, 0, 7), g = bits(lo, 32, 39), b = bits(hi, 0, 7), a = bits(hi, 32, 39);
    walker.state.rgba = r | (g << 8) | (b << 16) | (a << 24) | (static_cast<std::uint64_t>(walker.q_latch_bits) << 32);
    pending.wrote_register = true;
    break;
  }
  case 0x2: {
    walker.state.st = lo;
    walker.last_u = as_float(static_cast<std::uint32_t>(lo & 0xFFFFFFFFu));
    walker.last_v = as_float(static_cast<std::uint32_t>((lo >> 32) & 0xFFFFFFFFu));
    walker.q_latch_bits = static_cast<std::uint32_t>(hi & 0xFFFFFFFFu);
    walker.uv_known = true;
    walker.uv_from_st = true;
    pending.wrote_register = true;
    break;
  }
  case 0x3: {
    walker.state.uv = bits(lo, 0, 13) | (static_cast<std::uint64_t>(bits(lo, 32, 45)) << 16);
    walker.last_u = static_cast<float>(bits(lo, 0, 13));
    walker.last_v = static_cast<float>(bits(lo, 32, 45));
    walker.uv_known = true;
    walker.uv_from_st = false;
    pending.wrote_register = true;
    break;
  }
  case 0x4:  // XYZF2
  case 0x5:  // XYZ2
  case 0xC:  // XYZF3 (latch)
  case 0xD: {  // XYZ3 (latch)
    const bool with_fog = descriptor == 0x4 || descriptor == 0xC;
    const std::uint32_t raw_x = bits(lo, 0, 15);
    const std::uint32_t raw_y = bits(lo, 32, 47);
    const std::uint32_t z = with_fog ? bits(hi, 4, 27) : bits(hi, 0, 31);
    const std::uint32_t fog = with_fog ? bits(hi, 36, 43) : 0;
    const bool adc = bits(hi, 47, 47) != 0;
    const bool drawing = (descriptor == 0x4 || descriptor == 0x5) && !adc;
    add_vertex(walker, raw_x, raw_y, z, fog, with_fog, drawing);
    pending.kicked_vertices = true;
    break;
  }
  case 0x6:
  case 0x7: walker.state.tex0 = lo; pending.wrote_register = true; break;
  case 0x8:
  case 0x9: walker.state.clamp = lo; pending.wrote_register = true; break;
  case 0xA:
    walker.state.fog = static_cast<std::uint64_t>(bits(hi, 36, 43)) << 56;
    pending.wrote_register = true;
    break;
  case 0xE: {  // A+D: address in the high qword's low 7 bits, data is the low qword
    const std::uint32_t address = bits(hi, 0, 6);
    pending.wrote_register = true;
    writer.census.ad_writes += 1;
    writer.frame(pending.start_vblank, 0).ad_writes += 1;
    switch (address) {
    case 0x04:
    case 0x05:
    case 0x0C:
    case 0x0D: {
      const bool with_fog = address == 0x04 || address == 0x0C;
      const std::uint32_t raw_x = bits(lo, 0, 15);
      const std::uint32_t raw_y = bits(lo, 16, 31);
      const std::uint32_t z = with_fog ? bits(lo, 32, 55) : bits(lo, 32, 63);
      const std::uint32_t fog = with_fog ? bits(lo, 56, 63) : 0;
      const bool drawing = address == 0x04 || address == 0x05;
      add_vertex(walker, raw_x, raw_y, z, fog, with_fog, drawing);
      pending.kicked_vertices = true;
      break;
    }
    default:
      write_register(writer, walker, address, lo);
      break;
    }
    break;
  }
  default:  // 0xB, 0xF NOP
    break;
  }
}

void consume_reglist(CensusWriter &writer, PathWalker &walker, std::uint32_t descriptor, std::uint64_t word) {
  Pending &pending = walker.pending;
  switch (descriptor) {
  case 0x0: {
    const std::uint32_t prim = static_cast<std::uint32_t>(word & 0x7FFu);
    if (walker.batch_open && !walker.run_vertices.empty()) finish_run(writer, walker);
    walker.state.prim = prim;
    walker.prim_known = true;
    writer.census.prim_kicks += 1;
    writer.frame(pending.start_vblank, 0).prim_kicks += 1;
    pending.wrote_register = true;
    break;
  }
  case 0x1: {
    const std::uint64_t r = word & 0xFFu, g = (word >> 8) & 0xFFu, b = (word >> 16) & 0xFFu, a = (word >> 24) & 0xFFu;
    walker.state.rgba = r | (g << 8) | (b << 16) | (a << 24) | (static_cast<std::uint64_t>(walker.q_latch_bits) << 32);
    pending.wrote_register = true;
    break;
  }
  case 0x2: {
    walker.state.st = word;
    walker.last_u = as_float(static_cast<std::uint32_t>(word & 0xFFFFFFFFu));
    walker.last_v = as_float(static_cast<std::uint32_t>((word >> 32) & 0xFFFFFFFFu));
    walker.uv_known = true;
    walker.uv_from_st = true;
    pending.wrote_register = true;
    break;
  }
  case 0x3: {
    walker.state.uv = word;
    walker.last_u = static_cast<float>(word & 0xFFFFu) / 16.0f;
    walker.last_v = static_cast<float>((word >> 16) & 0xFFFFu) / 16.0f;
    walker.uv_known = true;
    walker.uv_from_st = false;
    pending.wrote_register = true;
    break;
  }
  case 0x4:
  case 0x5:
  case 0xC:
  case 0xD: {
    const bool with_fog = descriptor == 0x4 || descriptor == 0xC;
    const std::uint32_t raw_x = static_cast<std::uint32_t>(word & 0xFFFFu);
    const std::uint32_t raw_y = static_cast<std::uint32_t>((word >> 16) & 0xFFFFu);
    const std::uint32_t z = with_fog ? static_cast<std::uint32_t>((word >> 32) & 0xFFFFFFu)
                                     : static_cast<std::uint32_t>((word >> 32) & 0xFFFFFFFFu);
    const std::uint32_t fog = with_fog ? static_cast<std::uint32_t>((word >> 56) & 0xFFu) : 0;
    const bool drawing = descriptor == 0x4 || descriptor == 0x5;
    add_vertex(walker, raw_x, raw_y, z, fog, with_fog, drawing);
    pending.kicked_vertices = true;
    break;
  }
  case 0x6:
  case 0x7: walker.state.tex0 = word; pending.wrote_register = true; break;
  case 0x8:
  case 0x9: walker.state.clamp = word; pending.wrote_register = true; break;
  case 0xA: walker.state.fog = word; pending.wrote_register = true; break;
  case 0xE:
  case 0xF:  // A+D/NOP in REGLIST: no effect, counted
    writer.census.unsupported_reglist_descriptor += 1;
    break;
  default:
    write_register(writer, walker, descriptor, word);
    pending.wrote_register = true;
    break;
  }
}

void finish_packet(CensusWriter &writer, PathWalker &walker) {
  Pending &pending = walker.pending;
  if (!pending.active) return;
  finish_run(writer, walker);
  GsFrameCensus &row = writer.frame(pending.start_vblank, 0);
  row.packets += 1;
  writer.census.packets += 1;
  if (pending.tag.format == 0) {
    row.packed_tags += 1;
    writer.census.packed_tags += 1;
  } else if (pending.tag.format == 1) {
    row.reglist_tags += 1;
    writer.census.reglist_tags += 1;
  } else if (pending.tag.format == 2) {
    row.image_tags += 1;
    writer.census.image_tags += 1;
  } else {
    row.disabled_tags += 1;
    writer.census.disabled_tags += 1;
  }
  if (pending.kicked_vertices) {
    row.primitive_packets += 1;
    writer.census.primitive_packets += 1;
  } else if (pending.wrote_register) {
    row.register_packets += 1;
    writer.census.register_packets += 1;
  } else if (pending.tag.format == 2) {
    // An IMAGE packet is classified by its upload entry, not as a register or primitive packet.
  } else {
    row.empty_packets += 1;
    writer.census.empty_packets += 1;
  }
  if (pending.tag.format == 2) {
    GsDecodeImageUpload upload;
    upload.vblank = pending.start_vblank;
    upload.sequence = pending.start_sequence;
    upload.path = pending.path;
    upload.bytes = 0;
    upload.qwords = pending.tag.nloop;
    upload.has_transfer_state = walker.transfer_state_known;
    if (walker.transfer_state_known) {
      upload.dbp = static_cast<std::uint32_t>((walker.bitbltbuf >> 32) & 0x3FFFu);
      upload.dbw = static_cast<std::uint32_t>((walker.bitbltbuf >> 48) & 0x3Fu);
      upload.dpsm = static_cast<std::uint32_t>((walker.bitbltbuf >> 56) & 0x3Fu);
      upload.sbp = static_cast<std::uint32_t>(walker.bitbltbuf & 0x3FFFu);
      upload.sbw = static_cast<std::uint32_t>((walker.bitbltbuf >> 16) & 0x3Fu);
      upload.spsm = static_cast<std::uint32_t>((walker.bitbltbuf >> 24) & 0x3Fu);
      upload.dsax = static_cast<std::uint32_t>((walker.trxpos >> 32) & 0x7FFu);
      upload.dsay = static_cast<std::uint32_t>((walker.trxpos >> 48) & 0x7FFu);
      upload.rrw = static_cast<std::uint32_t>(walker.trxreg & 0xFFFu);
      upload.rrh = static_cast<std::uint32_t>((walker.trxreg >> 32) & 0xFFFu);
      upload.trxdir = static_cast<std::uint32_t>(walker.trxdir & 3u);
    } else {
      writer.census.image_without_transfer_state += 1;
    }
    upload.bytes = pending.image_bytes;
    upload.data = std::move(pending.image_data);
    writer.census.image_uploads += 1;
    writer.census.image_bytes += upload.bytes;
    row.image_uploads += 1;
    row.image_bytes += upload.bytes;
    writer.uploads.push_back(upload);
  }
  pending = Pending{};
}

// Feeds one record's payload into the per-path walker.
void feed_record(CensusWriter &writer, PathWalker &walker, const std::uint8_t *payload, std::uint32_t bytes,
                 const GsStreamFileRecord &record) {
  std::uint32_t at = 0;
  while (at + 16 <= bytes) {
    if (!walker.pending.active) {
      const std::uint64_t lo = read_u64(payload + at);
      const std::uint64_t hi = read_u64(payload + at + 8);
      at += 16;
      Pending pending;
      pending.active = true;
      pending.tag = decode_tag(lo, hi);
      pending.start_vblank = record.vblank;
      pending.start_sequence = record.sequence;
      pending.start_job = record.last_job;
      pending.start_record = static_cast<std::uint32_t>(record.sequence);
      pending.path = record.path;
      pending.tag_begin_offset = at - 16;
      if (pending.tag.format == 2) {
        pending.remaining = pending.tag.nloop;
        // An IMAGE packet's data size is NLOOP qwords; its payload bytes are counted as they arrive.
        pending.image_counted = false;
      } else if (pending.tag.format == 1) {
        pending.remaining = pending.tag.nloop * pending.tag.nreg;
      } else if (pending.tag.format == 0) {
        pending.remaining = pending.tag.nloop * pending.tag.nreg;
      } else {
        pending.remaining = 0;
      }
      walker.pending = pending;
      walker.q_latch_bits = 0x3F800000u;
      if (pending.tag.format == 0 && pending.tag.pre && pending.tag.prim != 0) {
        if (walker.batch_open && !walker.run_vertices.empty()) finish_run(writer, walker);
        walker.state.prim = pending.tag.prim;
        walker.prim_known = true;
        writer.census.prim_kicks += 1;
        writer.frame(record.vblank, 0).prim_kicks += 1;
      }
      if (pending.tag.nloop == 0) {
        finish_packet(writer, walker);
        continue;
      }
      continue;
    }
    Pending &pending = walker.pending;
    if (pending.tag.format == 2) {
      const std::uint32_t available = (bytes - at) / 16u;
      const std::uint32_t take = std::min(pending.remaining, available);
      if (take != 0) {
        pending.image_counted = true;
        // The upload entry is created at the end of the packet; keep a running byte count on the pending state and, for
        // a host->local transfer, the payload itself (S3 reconstructs the GS local memory from it).
        pending.image_bytes += take * 16ull;
        if ((walker.trxdir & 3u) == 0u)
          pending.image_data.insert(pending.image_data.end(), payload + at, payload + at + take * 16u);
        at += take * 16u;
        pending.remaining -= take;
      }
      if (pending.remaining == 0) finish_packet(writer, walker);
      if (take == 0) break;
      continue;
    }
    if (pending.tag.format == 3) {
      finish_packet(writer, walker);
      continue;
    }
    const std::uint64_t lo = read_u64(payload + at);
    const std::uint64_t hi = read_u64(payload + at + 8);
    at += 16;
    if (pending.tag.format == 0) {
      const std::uint32_t descriptor = pending.tag.regs[pending.descriptor];
      pending.descriptor = (pending.descriptor + 1u) % pending.tag.nreg;
      pending.remaining -= 1;
      consume_packed(writer, walker, descriptor, lo, hi);
    } else {
      for (int half = 0; half < 2 && pending.remaining > 0; ++half) {
        const std::uint32_t descriptor = pending.tag.regs[pending.descriptor];
        pending.descriptor = (pending.descriptor + 1u) % pending.tag.nreg;
        pending.remaining -= 1;
        consume_reglist(writer, walker, descriptor, half == 0 ? lo : hi);
      }
    }
    if (pending.remaining == 0) finish_packet(writer, walker);
  }
}

}  // namespace

bool census_gs_stream(const std::vector<std::uint8_t> &data, const GsStreamFileHeader &header,
                      const std::vector<GsStreamFileRecord> &records, GsStreamCensus &census,
                      std::vector<GsDecodeBatch> &batches, std::vector<GsDecodeImageUpload> &uploads,
                      std::string &error) {
  (void)header;
  census = GsStreamCensus{};
  batches.clear();
  uploads.clear();
  CensusWriter writer{census, batches, uploads};
  std::uint64_t last_vblank = 0;
  bool have_vblank = false;
  for (const GsStreamFileRecord &record : records) {
    census.records += 1;
    if (record.kind == kGsStreamKindFrameBegin || record.kind == kGsStreamKindFrameEnd) {
      if (!have_vblank || record.vblank != last_vblank) {
        last_vblank = record.vblank;
        have_vblank = true;
        writer.frame(record.vblank, 0);
      }
      continue;
    }
    if (record.kind != kGsStreamKindGif) continue;
    if (record.path < 1 || record.path > 3) {
      error = "GIF record with a path outside 1..3";
      return false;
    }
    census.gif_records[record.path] += 1;
    census.gif_bytes[record.path] += record.bytes;
    GsFrameCensus &row = writer.frame(record.vblank, 0);
    row.gif_records[record.path] += 1;
    row.gif_bytes[record.path] += record.bytes;
    if ((record.flags & kGsStreamFlagSignals) != 0) {
      census.signals += 1;
      row.signals += 1;
    }
    if (record.offset > data.size() || record.bytes > data.size() - record.offset) {
      error = "GIF record payload runs past the file image";
      return false;
    }
    PathWalker &walker = writer.paths[record.path];
    if (walker.pending.active) census.split_packets += 1;
    feed_record(writer, walker, data.data() + record.offset, record.bytes, record);
    if (walker.pending.active && walker.pending.tag.format == 2 && walker.pending.image_bytes != 0) {
      // The packet started in an earlier record; its upload entry is completed by the next record's data.
    }
  }
  // A packet left open at the end of the capture is a named refusal, never a silent truncation.
  for (std::uint32_t path = 1; path <= 3; ++path) {
    PathWalker &walker = writer.paths[path];
    if (!walker.pending.active) continue;
    census.truncated_packets += 1;
    if (walker.pending.tag.format == 0 || walker.pending.tag.format == 1) {
      // A truncated primitive tag: keep the vertices it did deliver, marked by the packet refusal.
      finish_run(writer, walker);
    } else if (walker.pending.tag.format == 2) {
      census.image_bytes += walker.pending.image_bytes;
    }
    walker.pending = Pending{};
  }
  return true;
}

bool find_wet_composite(const std::vector<GsDecodeBatch> &batches, std::uint64_t scene_vblank,
                        GsWetCompositeInfo &info) {
  info = GsWetCompositeInfo{};
  info.scene_vblank = scene_vblank;
  info.stream_vblank = scene_vblank + 1;
  const GsDecodeBatch *composite = nullptr;
  std::vector<const GsDecodeBatch *> clears;
  std::vector<const GsDecodeBatch *> darkens;
  std::vector<const GsDecodeBatch *> brightens;
  std::uint64_t full_screen = 0;
  for (const GsDecodeBatch &batch : batches) {
    if (batch.vblank != info.stream_vblank) continue;
    if (!batch.full_screen || batch.drawing_vertices < 2) continue;
    full_screen += 1;
    const GsDecodeState &state = batch.state;
    const bool clear = batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 0 && !state.prim_abe() &&
                       state.frame_fbp() == 210 && state.frame_fbw() == 10 && state.frame_psm() == 0 &&
                       state.frame_fbmsk() == 0x00FFFFFFu && state.tex0_tbp0() == 0x1A40u &&
                       state.test_ate() == 0 && ((state.rgba >> 24) & 0xFFu) == 0;
    const bool is_composite = batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 1 && state.prim_abe() &&
                              state.frame_fbp() == 210 && state.frame_fbw() == 10 && state.frame_psm() == 0 &&
                              state.frame_fbmsk() == 0 && state.tex0_tbp0() == 0x3140u && state.tex0_psm() == 0 &&
                              state.tex0_tw() == 7 && state.tex0_th() == 7 && state.tex0_tcc() == 1 &&
                              state.tex0_tfx() == 0 && state.tex1_mmag() == 1 && state.tex1_mmin() == 1 &&
                              state.tex1_mxl() == 0 && state.clamp_wms() == 2 && state.clamp_wmt() == 2 &&
                              state.clamp_minu() == 0 && state.clamp_maxu() == 127 && state.clamp_minv() == 0 &&
                              state.clamp_maxv() == 127 && state.alpha_a() == 0 && state.alpha_b() == 2 &&
                              state.alpha_c() == 1 && state.alpha_d() == 1 && state.test_ate() == 0 &&
                              state.zbuf_zmsk() == 1;
    const bool multiply = batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 0 && state.prim_abe() &&
                          state.frame_fbp() == 210 && state.frame_fbw() == 10 && state.frame_psm() == 0 &&
                          state.frame_fbmsk() == 0 && state.alpha_a() == 1 && state.alpha_b() == 2 &&
                          state.alpha_c() == 2 && state.alpha_d() == 2;
    if (clear) clears.push_back(&batch);
    else if (is_composite) {
      if (composite != nullptr) {
        info.refusal = "more than one full-screen wet composite (FBP 210, TBP0 0x3140, blend Cs*Ad+Cd)";
        return false;
      }
      composite = &batch;
    } else if (multiply && state.alpha_fix() == 127) darkens.push_back(&batch);
    else if (multiply && state.alpha_fix() == 255) brightens.push_back(&batch);
  }
  if (composite == nullptr) {
    info.refusal = "no full-screen wet composite (FBP 210, TBP0 0x3140, blend Cs*Ad+Cd) in the attributed stream";
    return false;
  }
  // The chain's alpha clear is the LAST matching clear before the composite: earlier identical clears are other
  // full-screen alpha writes of the same frame (seq 302 in the S2 capture), and the measured order puts the wet-road
  // clear immediately before the reflection mask job and the composite. They stay counted as unrecognised.
  for (const GsDecodeBatch *clear : clears)
    if (clear->sequence < composite->sequence && (info.alpha_clear == nullptr ||
                                                  clear->sequence > info.alpha_clear->sequence))
      info.alpha_clear = clear;
  if (info.alpha_clear == nullptr) {
    info.refusal = "no full-screen alpha clear (TBP0 0x1A40, FBMSK alpha only) before the wet composite";
    return false;
  }
  // The post chain's multiplies are the first darken and first brighten AFTER the composite; earlier ones (seq 318/341)
  // belong to effects before the post chain and stay counted.
  for (const GsDecodeBatch *darken : darkens)
    if (darken->sequence > composite->sequence &&
        (info.post_darken == nullptr || darken->sequence < info.post_darken->sequence))
      info.post_darken = darken;
  for (const GsDecodeBatch *brighten : brightens)
    if (brighten->sequence > composite->sequence &&
        (info.post_brighten == nullptr || brighten->sequence < info.post_brighten->sequence))
      info.post_brighten = brighten;
  info.composite = composite;
  std::uint64_t recognised = 1;  // the composite
  if (info.alpha_clear != nullptr) recognised += 1;
  if (info.post_darken != nullptr) recognised += 1;
  if (info.post_brighten != nullptr) recognised += 1;
  info.unrecognised_full_screen = full_screen - recognised;
  info.found = true;
  return true;
}

namespace {

// The measured wet-composite signature (S2), shared by the chain classifier and the S2 recogniser.
bool is_wet_composite_batch(const GsDecodeBatch &batch) {
  const GsDecodeState &state = batch.state;
  return batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 1 && state.prim_abe() &&
         state.frame_fbp() == 210 && state.frame_fbw() == 10 && state.frame_psm() == 0 && state.frame_fbmsk() == 0 &&
         state.tex0_tbp0() == 0x3140u && state.tex0_psm() == 0 && state.tex0_tw() == 7 && state.tex0_th() == 7 &&
         state.tex0_tcc() == 1 && state.tex0_tfx() == 0 && state.tex1_mmag() == 1 && state.tex1_mmin() == 1 &&
         state.tex1_mxl() == 0 && state.clamp_wms() == 2 && state.clamp_wmt() == 2 && state.clamp_minu() == 0 &&
         state.clamp_maxu() == 127 && state.clamp_minv() == 0 && state.clamp_maxv() == 127 &&
         state.alpha_a() == 0 && state.alpha_b() == 2 && state.alpha_c() == 1 && state.alpha_d() == 1 &&
         state.test_ate() == 0 && state.zbuf_zmsk() == 1;
}

bool is_full_screen_multiply_batch(const GsDecodeBatch &batch) {
  const GsDecodeState &state = batch.state;
  return batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 0 && state.prim_abe() &&
         state.frame_fbp() == 210 && state.frame_fbw() == 10 && state.frame_psm() == 0 && state.frame_fbmsk() == 0 &&
         state.alpha_a() == 1 && state.alpha_b() == 2 && state.alpha_c() == 2 && state.alpha_d() == 2;
}

// The full-screen sample of page 210 that opens the measured stream. It samples the framebuffer itself 1:1 with the
// vertex colour (128,77,0,128), but FRAME.FBMSK = 0xFFFFFFFF masks every colour lane; ZBUF.ZMSK = 0 with ZTST GEQUAL
// and z = 1 writes the frame's depth buffer. It is the frame's depth clear, not a colour grade.
bool is_frame_sample_batch(const GsDecodeBatch &batch) {
  const GsDecodeState &state = batch.state;
  const bool colour = (state.rgba & 0xFFFFFFFFu) == 0x80004D80u;  // R 128, G 77, B 0, A 128
  return batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 1 && state.prim_abe() && colour &&
         state.frame_fbp() == 210 && state.frame_fbw() == 10 && state.frame_psm() == 0 &&
         state.frame_fbmsk() == 0xFFFFFFFFu && state.tex0_tbp0() == 0x1A40u && state.tex0_psm() == 0 &&
         state.tex0_tw() == 10 && state.tex0_th() == 9 && state.tex0_tcc() == 1 && state.tex0_tfx() == 0 &&
         state.tex1_mmag() == 0 && state.tex1_mmin() == 0 && state.tex1_mxl() == 0 && state.clamp_wms() == 2 &&
         state.clamp_wmt() == 2 && state.clamp_minu() == 0 && state.clamp_maxu() == 639 &&
         state.clamp_minv() == 0 && state.clamp_maxv() == 447 && state.alpha_a() == 0 && state.alpha_b() == 1 &&
         state.alpha_c() == 0 && state.alpha_d() == 1 && state.test_ate() == 1 && state.zbuf_zmsk() == 0;
}

// The CT16 present blit: page 210 sampled 1:1 with the neutral vertex colour (128,128,128,128), no blend, into one of
// the two CT16 display buffers (FBP 140 or FBP 0, FBW 10, PSM 2). This is the operation that produces the scanout image
// the reference dump holds; its DTHE/DIMX state is the captured dither.
bool is_present_batch(const GsDecodeBatch &batch) {
  const GsDecodeState &state = batch.state;
  const bool colour = (state.rgba & 0xFFFFFFFFu) == 0x80808080u;  // R 128, G 128, B 128, A 128
  const bool display_target = (state.frame_fbp() == 140 || state.frame_fbp() == 0) && state.frame_fbw() == 10 &&
                              state.frame_psm() == 2;
  return batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 1 && !state.prim_abe() && colour &&
         display_target && state.frame_fbmsk() == 0 &&
         state.tex0_tbp0() == 0x1A40u && state.tex0_psm() == 0 && state.tex0_tw() == 10 && state.tex0_th() == 9 &&
         state.tex0_tcc() == 1 && state.tex0_tfx() == 0 && state.tex1_mmag() == 0 && state.tex1_mmin() == 0 &&
         state.tex1_mxl() == 0 && state.clamp_wms() == 2 && state.clamp_wmt() == 2 && state.clamp_minu() == 0 &&
         state.clamp_maxu() == 639 && state.clamp_minv() == 0 && state.clamp_maxv() == 447 &&
         state.alpha_a() == 0 && state.alpha_b() == 1 && state.alpha_c() == 0 && state.alpha_d() == 1 &&
         state.test_ate() == 0 && state.zbuf_zmsk() == 1;
}

// A full-screen clear of one target: PRIM sprite, TME and ABE off, the black + alpha 0 vertex colour, and the given
// target/FBMSK/TEST signature. The pre-world clears and the two alpha-only clears share this shape; the target and the
// TEST distinguish them.
bool is_clear_batch(const GsDecodeBatch &batch, std::uint32_t fbp, std::uint32_t fbw, std::uint32_t psm,
                    std::uint32_t fbmsk, std::uint64_t test) {
  const GsDecodeState &state = batch.state;
  return batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 0 && !state.prim_abe() &&
         state.frame_fbp() == fbp && state.frame_fbw() == fbw && state.frame_psm() == psm &&
         state.frame_fbmsk() == fbmsk && (state.rgba & 0xFFFFFFFFu) == 0 && state.test == test;
}

}  // namespace

const char *gs_chain_op_name(GsChainOp op) noexcept {
  switch (op) {
  case GsChainOp::pre_world_clear_rgb: return "pre-world-clear-rgb";
  case GsChainOp::pre_world_clear_alpha: return "pre-world-clear-alpha";
  case GsChainOp::pre_world_clear_test: return "pre-world-clear-test";
  case GsChainOp::frame_sample: return "frame-sample";
  case GsChainOp::reflection_alpha_clear: return "reflection-alpha-clear";
  case GsChainOp::wet_composite: return "wet-composite";
  case GsChainOp::post_multiply: return "post-multiply";
  case GsChainOp::present_ct16: return "present-ct16";
  case GsChainOp::display_alpha_clear: return "display-alpha-clear";
  case GsChainOp::unknown: return "unknown";
  case GsChainOp::kChainOpCount: break;
  }
  return "unknown";
}

bool find_post_chain(const std::vector<GsDecodeBatch> &batches, std::uint64_t scene_vblank,
                     GsPostChainInfo &info) {
  info = GsPostChainInfo{};
  info.scene_vblank = scene_vblank;
  info.stream_vblank = scene_vblank + 1;
  std::vector<const GsDecodeBatch *> multiplies;
  std::uint64_t previous_sequence = 0;
  for (const GsDecodeBatch &batch : batches) {
    if (batch.vblank != info.stream_vblank) continue;
    if (!batch.full_screen || batch.drawing_vertices < 2) continue;
    if (batch.sequence < previous_sequence) {
      info.refusal = "the decoded batches are not in stream order";
      return false;
    }
    previous_sequence = batch.sequence;
    info.full_screen_batches += 1;
    GsChainEntry entry;
    entry.batch = &batch;
    const GsDecodeState &state = batch.state;
    if (is_frame_sample_batch(batch)) {
      entry.op = GsChainOp::frame_sample;
      entry.after_world = false;
      entry.note = "full-screen page-210 sample with vertex colour (128,77,0,128): FBMSK 0xFFFFFFFF masks every colour "
                   "lane while ZBUF.ZMSK 0 with ZTST GEQUAL writes z=1 over the depth buffer (the frame's depth clear); "
                   "it changes no colour and its depth is the world render's initial depth";
    } else if (is_wet_composite_batch(batch)) {
      entry.op = GsChainOp::wet_composite;
      entry.after_world = true;
      entry.note = "sphere-mapper composite (TBP0 0x3140), ALPHA 0x58 (Cs - 0) * Ad + Cd";
      if (info.composite != nullptr) {
        info.refusal = "more than one full-screen wet composite in the attributed stream";
        return false;
      }
      info.composite = &batch;
    } else if (is_full_screen_multiply_batch(batch)) {
      entry.op = GsChainOp::post_multiply;
      entry.after_world = true;
      char note[160];
      std::snprintf(note, sizeof note, "Cd * FIX %u / 128 full-screen multiply (ALPHA 0xA9)", state.alpha_fix());
      entry.note = note;
      multiplies.push_back(&batch);
    } else if (is_present_batch(batch)) {
      entry.op = GsChainOp::present_ct16;
      entry.after_world = true;
      char note[200];
      std::snprintf(note, sizeof note,
                    "page 210 -> FBP %u/FBW 10 CT16 present, vertex colour (128,128,128,128); DTHE %llu DIMX %016llX",
                    state.frame_fbp(), static_cast<unsigned long long>(state.dthe_on()),
                    static_cast<unsigned long long>(state.dimx));
      entry.note = note;
      if (info.present != nullptr) {
        info.refusal = "more than one full-screen CT16 present (FBP 140) in the attributed stream";
        return false;
      }
      info.present = &batch;
    } else if (is_clear_batch(batch, 140, 10, 2, 0x00FFFFFFu, 0) ||
               is_clear_batch(batch, 0, 10, 2, 0x00FFFFFFu, 0)) {
      entry.op = GsChainOp::display_alpha_clear;
      entry.after_world = true;
      entry.note = "alpha-only clear of the CT16 display buffer after the present; FBMSK 0x00FFFFFF keeps every RGB "
                   "bit, so the scanout RGB is unchanged";
      if (info.present != nullptr && batch.state.frame_fbp() != info.present->state.frame_fbp()) {
        info.refusal = "the display alpha clear and the present target different CT16 display buffers";
        return false;
      }
    } else if (is_clear_batch(batch, 210, 10, 0, 0x00FFFFFFu, 0)) {
      entry.op = GsChainOp::reflection_alpha_clear;
      entry.after_world = false;
      entry.note = "alpha-only clear immediately before the PassId 7 reflection draws; the world renderer applies it "
                   "at that position (clear_alpha_before_wet_reflection)";
      if (info.alpha_clear != nullptr) {
        info.refusal = "more than one pre-reflection alpha clear in the attributed stream";
        return false;
      }
      info.alpha_clear = &batch;
    } else if (is_clear_batch(batch, 210, 10, 0, 0, 0x30000u)) {
      entry.op = GsChainOp::pre_world_clear_rgb;
      entry.after_world = false;
      entry.note = "frame clear: black RGB + alpha 0 (FBMSK 0), before the frame's PATH1 draws";
    } else if (is_clear_batch(batch, 210, 10, 0, 0x00FFFFFFu, 0x30000u)) {
      entry.op = GsChainOp::pre_world_clear_alpha;
      entry.after_world = false;
      entry.note = "frame alpha-only clear (FBMSK 0x00FFFFFF), before the frame's PATH1 draws";
    } else if (is_clear_batch(batch, 210, 10, 0, 0, 0x32001u)) {
      entry.op = GsChainOp::pre_world_clear_test;
      entry.after_world = false;
      entry.note = "black frame clear under TEST 0x32001 (ATE + ZTST GEQUAL), before the frame's PATH1 draws";
    } else {
      entry.op = GsChainOp::unknown;
      entry.after_world = false;
      entry.note = "unrecognised full-screen batch: " + gs_batch_state_line(batch);
      info.unrecognised_full_screen += 1;
    }
    info.counts[static_cast<std::size_t>(entry.op)] += 1;
    info.entries.push_back(std::move(entry));
  }
  if (info.composite == nullptr) {
    info.refusal = "no full-screen wet composite (FBP 210, TBP0 0x3140, blend Cs*Ad+Cd) in the attributed stream";
    return false;
  }
  if (info.alpha_clear == nullptr) {
    info.refusal = "no pre-reflection alpha clear (FBP 210, FBMSK 0x00FFFFFF, TEST 0) in the attributed stream";
    return false;
  }
  if (multiplies.size() < 2) {
    info.refusal = "the captured post chain does not carry both multiplies (FIX 127 and FIX 255)";
    return false;
  }
  // The measured order: alpha clear, composite, FIX 127, FIX 255, present. Pin it; a different order is a refusal.
  for (const GsDecodeBatch *batch : multiplies) {
    if (batch->sequence < info.composite->sequence) continue;
    if (batch->state.alpha_fix() == 127 && info.post_darken == nullptr) info.post_darken = batch;
    if (batch->state.alpha_fix() == 255 && info.post_brighten == nullptr) info.post_brighten = batch;
  }
  if (info.post_darken == nullptr || info.post_brighten == nullptr) {
    info.refusal = "the captured post chain is missing a FIX 127 or FIX 255 multiply after the composite";
    return false;
  }
  if (info.present == nullptr) {
    info.refusal = "no full-screen CT16 present (FBP 140) in the attributed stream";
    return false;
  }
  if (!(info.alpha_clear->sequence < info.composite->sequence &&
        info.composite->sequence < info.post_darken->sequence &&
        info.post_darken->sequence < info.post_brighten->sequence &&
        info.post_brighten->sequence < info.present->sequence)) {
    info.refusal = "the chain draws are not in the measured stream order (alpha clear, composite, FIX 127, FIX 255, "
                   "present)";
    return false;
  }
  info.found = true;
  return true;
}

const char *gs_batch_class_name(const GsDecodeBatch &batch) noexcept {
  const GsDecodeState &state = batch.state;
  if (!batch.full_screen) return "batch";
  if (batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 0 && !state.prim_abe() &&
      state.frame_fbmsk() == 0x00FFFFFFu)
    return "full-screen-alpha-only";
  if (batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 1 && state.tex0_tbp0() == 0x3140u)
    return "full-screen-wet-composite";
  if (batch.path == 2 && state.prim_type() == 6 && state.prim_tme() == 0 && state.prim_abe() && state.alpha_c() == 2)
    return "full-screen-multiply";
  if (batch.path == 2 && state.prim_type() == 6 && state.tex0_tbp0() == 0x1A40u && state.frame_fbp() == 210)
    return "full-screen-frame-sample";
  if (batch.path == 2 && state.prim_type() == 6 && state.tex0_tbp0() == 0x1A40u && state.frame_fbp() == 0 &&
      state.frame_psm() == 2)
    return "full-screen-present-blit";
  if (state.tex0_tbp0() == 0x3140u) return "full-screen-sphere-source";
  return "full-screen-other";
}

std::string gs_batch_state_line(const GsDecodeBatch &batch) {
  const GsDecodeState &state = batch.state;
  char line[768];
  std::snprintf(line, sizeof line,
                "path %u vblank %llu seq %u prim %03X tme %u abe %u tbp0 %04X psm %02X tw %u th %u tcc %u tfx %u "
                "tex0 %016llX tex1 %016llX rgba %08X frame fbp %u fbw %u psm %u fbmsk %08X test %016llX "
                "alpha %016llX zbuf %016llX clamp %016llX scissor %016llX xyoff %016llX "
                "extent %.1f..%.1f x %.1f..%.1f verts %u",
                static_cast<unsigned>(batch.path), static_cast<unsigned long long>(batch.vblank), batch.sequence,
                state.prim_value(), state.prim_tme(), state.prim_abe(), state.tex0_tbp0(), state.tex0_psm(),
                state.tex0_tw(), state.tex0_th(), state.tex0_tcc(), state.tex0_tfx(),
                static_cast<unsigned long long>(state.tex0),
                static_cast<unsigned long long>(state.tex1),
                static_cast<std::uint32_t>(state.rgba & 0xFFFFFFFFu),
                state.frame_fbp(), state.frame_fbw(), state.frame_psm(), state.frame_fbmsk(),
                static_cast<unsigned long long>(state.test),
                static_cast<unsigned long long>(state.alpha),
                static_cast<unsigned long long>(state.zbuf), static_cast<unsigned long long>(state.clamp),
                static_cast<unsigned long long>(state.scissor), static_cast<unsigned long long>(state.xyoffset),
                batch.min_x, batch.max_x, batch.min_y, batch.max_y, batch.vertices);
  return line;
}

}  // namespace gt4::coregs3
