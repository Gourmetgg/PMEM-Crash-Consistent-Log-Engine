#include "PersistentLinkedList.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <system_error>
#include <unordered_set>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "CacheFlush.h"

namespace pmem {

namespace {
constexpr std::size_t kMinimumMappedBytes = kPmemNodeRegionOffset + sizeof(PersistentNode) * 16;
}  // namespace

PersistentLinkedList::PersistentLinkedList(const std::string& mapped_file,
                                           std::size_t mapped_size_bytes)
    : mapped_file_(mapped_file),
      mapped_size_bytes_(std::max(mapped_size_bytes, kMinimumMappedBytes)) {
    fd_ = ::open(mapped_file_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open mapped file failed");
    }

    if (::ftruncate(fd_, static_cast<off_t>(mapped_size_bytes_)) != 0) {
        const int err = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::system_error(err, std::generic_category(), "ftruncate mapped file failed");
    }

    mapped_base_ = ::mmap(nullptr,
                          mapped_size_bytes_,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED,
                          fd_,
                          0);
    if (mapped_base_ == MAP_FAILED) {
        const int err = errno;
        mapped_base_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        throw std::system_error(err, std::generic_category(), "mmap failed");
    }

    header_ = static_cast<PersistentListHeader*>(mapped_base_);
    initialize_region();
}

PersistentLinkedList::~PersistentLinkedList() {
    if (mapped_base_ != nullptr) {
        ::munmap(mapped_base_, mapped_size_bytes_);
        mapped_base_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void PersistentLinkedList::initialize_region() {
    if (header_->magic == kPersistentListMagic) {
        return;
    }

    std::memset(mapped_base_, 0, mapped_size_bytes_);
    header_->magic = kPersistentListMagic;
    header_->head_offset.store(kInvalidOffset, std::memory_order_release);
    header_->allocated_offset.store(kPmemNodeRegionOffset, std::memory_order_release);
    header_->recovery_epoch.store(0, std::memory_order_release);
    persist_range(header_, sizeof(PersistentListHeader));
}

void PersistentLinkedList::persist_range(const void* addr, std::size_t len) const noexcept {
    if (addr == nullptr || len == 0) {
        return;
    }

    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(addr);
    const std::uintptr_t end = begin + len;
    std::uintptr_t line = begin & ~(static_cast<std::uintptr_t>(kCacheLineSize - 1));

    while (line < end) {
        clflush(reinterpret_cast<const void*>(line));
        line += kCacheLineSize;
    }
    sfence();
}

bool PersistentLinkedList::is_valid_node_offset(std::uint64_t offset,
                                                std::uint64_t allocated_limit) const noexcept {
    if (offset == kInvalidOffset) {
        return false;
    }
    if (offset < kPmemNodeRegionOffset) {
        return false;
    }
    if (offset % sizeof(PersistentNode) != 0) {
        return false;
    }
    if (offset + sizeof(PersistentNode) > allocated_limit) {
        return false;
    }
    if (offset + sizeof(PersistentNode) > mapped_size_bytes_) {
        return false;
    }
    return true;
}

std::optional<std::uint64_t> PersistentLinkedList::allocate_node_offset() {
    // TODO(intel-lab): implement lock-free persistent allocator with failure atomicity.
    return std::nullopt;
}

PersistentNode* PersistentLinkedList::offset_to_node(std::uint64_t offset) const noexcept {
    if (offset + sizeof(PersistentNode) > mapped_size_bytes_) {
        return nullptr;
    }
    auto* base = static_cast<std::byte*>(mapped_base_);
    return reinterpret_cast<PersistentNode*>(base + offset);
}

std::uint32_t PersistentLinkedList::compute_checksum(std::uint64_t value) const noexcept {
    const std::uint64_t mixed = value ^ (value >> 33U) ^ 0x9E3779B97F4A7C15ULL;
    return static_cast<std::uint32_t>(mixed & 0xFFFFFFFFULL);
}

bool PersistentLinkedList::insert(std::uint64_t value) {
    (void)value;
    // TODO(intel-lab): core insert protocol intentionally not implemented.
    // Required protocol:
    // 1) append undo/redo intent atomically,
    // 2) persist node payload before publish,
    // 3) CAS-publish head pointer,
    // 4) persist publish and transaction commit marker.
    return false;
}

bool PersistentLinkedList::recover() {
    // TODO(intel-lab): implement log replay / rollback policy.
    header_->recovery_epoch.fetch_add(1, std::memory_order_acq_rel);
    persist_range(&header_->recovery_epoch, sizeof(header_->recovery_epoch));
    return true;
}

IntegrityReport PersistentLinkedList::validate() const {
    IntegrityReport report{};
    if (header_->magic != kPersistentListMagic) {
        report.has_out_of_range_pointer = true;
        return report;
    }

    const std::uint64_t allocated_limit =
        std::min<std::uint64_t>(header_->allocated_offset.load(std::memory_order_acquire),
                                static_cast<std::uint64_t>(mapped_size_bytes_));
    const std::size_t max_possible_nodes =
        (allocated_limit > kPmemNodeRegionOffset)
            ? static_cast<std::size_t>((allocated_limit - kPmemNodeRegionOffset) / sizeof(PersistentNode))
            : 0;

    std::unordered_set<std::uint64_t> visited;
    std::uint64_t current = header_->head_offset.load(std::memory_order_acquire);

    while (current != kInvalidOffset) {
        if (!is_valid_node_offset(current, allocated_limit)) {
            report.has_out_of_range_pointer = true;
            break;
        }
        if (!visited.insert(current).second) {
            report.has_cycle = true;
            break;
        }
        report.reachable_nodes += 1;
        if (report.reachable_nodes > max_possible_nodes) {
            report.has_cycle = true;
            break;
        }

        const PersistentNode* node = offset_to_node(current);
        if (node == nullptr) {
            report.has_out_of_range_pointer = true;
            break;
        }

        current = node->next_offset.load(std::memory_order_acquire);
    }

    return report;
}

std::vector<std::uint64_t> PersistentLinkedList::snapshot(std::size_t max_nodes) const {
    (void)max_nodes;
    // TODO(intel-lab): return only durable, fully committed transaction values.
    return {};
}

void PersistentLinkedList::reset() {
    header_->head_offset.store(kInvalidOffset, std::memory_order_release);
    header_->allocated_offset.store(kPmemNodeRegionOffset, std::memory_order_release);
    header_->recovery_epoch.fetch_add(1, std::memory_order_acq_rel);

    auto* base = static_cast<std::byte*>(mapped_base_);
    std::memset(base + kPmemNodeRegionOffset, 0, mapped_size_bytes_ - kPmemNodeRegionOffset);

    persist_range(header_, sizeof(PersistentListHeader));
    persist_range(base + kPmemNodeRegionOffset, mapped_size_bytes_ - kPmemNodeRegionOffset);
}

}  // namespace pmem
