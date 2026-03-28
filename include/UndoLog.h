#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pmem {

constexpr std::size_t kCacheLineSize = 64;

enum class UndoLogState : std::uint32_t {
    kEmpty = 0,
    kPrepared = 1,
    kCommitted = 2,
};

// Lightweight undo-log record with strict cacheline alignment.
// The fixed 64-byte layout reduces risk of torn writes that cross cache lines.
struct alignas(kCacheLineSize) UndoLogEntry {
    std::atomic<std::uint64_t> target_offset;
    std::atomic<std::uint64_t> before_value;
    std::atomic<std::uint32_t> checksum;
    std::atomic<UndoLogState> state;
    std::array<std::byte, 64 - (8 + 8 + 4 + 4)> reserved;
};

static_assert(alignof(UndoLogEntry) == kCacheLineSize, "UndoLogEntry must be 64-byte aligned");
static_assert(sizeof(UndoLogEntry) == kCacheLineSize, "UndoLogEntry must be one cache line");

}  // namespace pmem
