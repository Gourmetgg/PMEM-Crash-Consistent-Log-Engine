#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "UndoLog.h"

namespace pmem {

constexpr std::uint64_t kInvalidOffset = 0;
constexpr std::uint64_t kPersistentListMagic = 0x504d454d4c4c4953ULL;  // "PMEMLLIS"
constexpr std::size_t kPmemNodeRegionOffset = 4096;

enum class NodeState : std::uint32_t {
    kFree = 0,
    kPrepared = 1,
    kCommitted = 2,
};

// Persistent list node: one cacheline to reduce torn-write risk.
struct alignas(kCacheLineSize) PersistentNode {
    std::atomic<std::uint64_t> next_offset;
    std::atomic<std::uint64_t> value;
    std::atomic<std::uint32_t> checksum;
    std::atomic<NodeState> state;
    std::array<std::byte, kCacheLineSize - (8 + 8 + 4 + 4)> reserved;
};

// Persistent metadata header.
struct alignas(kCacheLineSize) PersistentListHeader {
    std::uint64_t magic;
    std::atomic<std::uint64_t> head_offset;
    std::atomic<std::uint64_t> allocated_offset;
    std::atomic<std::uint64_t> recovery_epoch;
    std::array<std::byte, kCacheLineSize - (8 + 8 + 8 + 8)> reserved;
};

static_assert(sizeof(PersistentNode) == kCacheLineSize, "PersistentNode must fit one cacheline");
static_assert(alignof(PersistentNode) == kCacheLineSize, "PersistentNode must be cacheline aligned");
static_assert(sizeof(PersistentListHeader) == kCacheLineSize, "PersistentListHeader must fit one cacheline");
static_assert(alignof(PersistentListHeader) == kCacheLineSize,
              "PersistentListHeader must be cacheline aligned");

struct IntegrityReport {
    bool has_cycle = false;
    bool has_out_of_range_pointer = false;
    std::size_t reachable_nodes = 0;
};

class PersistentLinkedList {
public:
    PersistentLinkedList(const std::string& mapped_file, std::size_t mapped_size_bytes);
    ~PersistentLinkedList();

    PersistentLinkedList(const PersistentLinkedList&) = delete;
    PersistentLinkedList& operator=(const PersistentLinkedList&) = delete;
    PersistentLinkedList(PersistentLinkedList&&) = delete;
    PersistentLinkedList& operator=(PersistentLinkedList&&) = delete;

    // Contract:
    // 1) New node is persisted before publish.
    // 2) Head pointer publish is atomic.
    // 3) Publish is followed by persistence barrier.
    bool insert(std::uint64_t value);

    // Recovery entry point. Core replay/rollback algorithm is TODO in this scaffold.
    bool recover();

    IntegrityReport validate() const;
    std::vector<std::uint64_t> snapshot(std::size_t max_nodes) const;
    void reset();

private:
    void initialize_region();
    void persist_range(const void* addr, std::size_t len) const noexcept;
    bool is_valid_node_offset(std::uint64_t offset, std::uint64_t allocated_limit) const noexcept;
    std::optional<std::uint64_t> allocate_node_offset();
    PersistentNode* offset_to_node(std::uint64_t offset) const noexcept;
    std::uint32_t compute_checksum(std::uint64_t value) const noexcept;

    std::string mapped_file_;
    std::size_t mapped_size_bytes_;
    int fd_ = -1;
    void* mapped_base_ = nullptr;
    PersistentListHeader* header_ = nullptr;
};

}  // namespace pmem
