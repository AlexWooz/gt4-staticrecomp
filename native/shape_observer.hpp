#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "game_target.hpp"

namespace gt4::observation {

// TASK-100 S3: guest addresses from the per-target table (a target without them gets kNeverMatch, so the observer
// never triggers there).
inline constexpr std::uint32_t shape_call_pc =
    ::gt4::target::match_or_never(::gt4::target::active().hooks.shape_call_pc);
inline constexpr std::uint32_t shape_target_pc =
    ::gt4::target::match_or_never(::gt4::target::active().hooks.shape_target_pc);
inline constexpr std::uint32_t shape_return_pc =
    ::gt4::target::match_or_never(::gt4::target::active().hooks.shape_return_pc);
inline constexpr std::uint32_t shape_state_address = 0x70002000;

struct MemoryView {
  std::span<const std::byte> ram;
  std::span<const std::byte> scratchpad;
};

bool copy_guest(MemoryView memory, std::uint32_t address,
                std::span<std::byte> destination) noexcept;

enum SampleField : std::uint32_t {
  flag_valid = 1, enabled_valid = 2, cached_bits_valid = 4,
  cursor_valid = 8, shape_bits_valid = 16, count_valid = 32,
  all_fields_valid = 63
};

struct Metadata {
  std::uint32_t valid_fields = 0;
  std::uint32_t enabled = 0;
  std::uint32_t cached_bits = 0;
  std::uint32_t cursor = 0;
  std::uint32_t shape_bits = 0;
  std::uint16_t descriptor_count = 0;
  std::uint8_t flag = 0;
  bool operator==(const Metadata &) const = default;
};

Metadata sample_metadata(MemoryView memory, std::uint32_t shape_address) noexcept;

struct Inputs {
  std::array<std::array<std::uint32_t, 4>, 4> argument_bits{};
  std::uint32_t fpr12_bits = 0;
  std::uint32_t guest_sp = 0;
  Metadata metadata{};
  bool operator==(const Inputs &) const = default;
};

struct CompletedCall {
  std::uint64_t epoch = 0;
  std::uint64_t incarnation = 0;
  std::uint64_t sequence = 0;
  std::uint32_t owner_id = 0;
  Inputs before{};
  Metadata after{};
};

struct Counters {
  std::uint64_t started = 0, completed = 0, aborted = 0;
  std::uint64_t handler_skipped = 0, orphan = 0, rejected_return = 0;
  std::uint64_t ambiguous_stack = 0, capacity_rejected = 0;
  std::uint64_t record_dropped = 0, incomplete_samples = 0;
};

// Single EE owner only; inspect/copy records off execution before start() clears them.
// Owner IDs are opaque; the runtime adapter must reject idle/unknown kernel owners.
class ShapeObserver {
public:
  static constexpr std::size_t owner_limit = 64;
  static constexpr std::size_t pending_per_owner = 16;
  static constexpr std::size_t record_limit = 256;

  void start() noexcept;
  void stop() noexcept;
  void reset_epoch() noexcept;
  void invalidate_owner(std::uint32_t owner_id) noexcept;
  void begin(std::uint32_t owner_id, bool in_handler, const Inputs &inputs) noexcept;
  void complete(std::uint32_t owner_id, bool in_handler, std::uint32_t return_pc,
                std::uint32_t guest_sp, const Metadata &after) noexcept;
  void complete_from_memory(std::uint32_t owner_id, bool in_handler,
                            std::uint32_t return_pc, std::uint32_t guest_sp,
                            MemoryView memory) noexcept;
  bool enabled() const noexcept { return enabled_; }
  const Counters &counters() const noexcept { return counters_; }
  std::span<const CompletedCall> records() const noexcept {
    return {records_.data(), record_count_};
  }

private:
  struct Owner {
    bool occupied = false;
    std::uint32_t id = 0;
    std::uint64_t incarnation = 0;
    std::size_t count = 0;
    std::array<CompletedCall, pending_per_owner> pending{};
  };
  Owner *find_owner(std::uint32_t owner_id) noexcept;
  bool advance(std::uint64_t &identifier) noexcept;
  std::array<Owner, owner_limit> owners_{};
  std::array<CompletedCall, record_limit> records_{};
  Counters counters_{};
  std::uint64_t epoch_ = 0, next_incarnation_ = 0, next_sequence_ = 0;
  std::size_t record_count_ = 0;
  bool enabled_ = false;
};

} // namespace gt4::observation
