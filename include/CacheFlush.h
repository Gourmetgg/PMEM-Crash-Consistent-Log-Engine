#pragma once

#include <cstddef>

namespace pmem {

// TODO(intel-lab): implement CLFLUSH wrapper with inline assembly.
inline void clflush(const void* /*addr*/) noexcept {
    // Intentionally left blank: core persistence primitive is not implemented in this scaffold.
}

// TODO(intel-lab): implement SFENCE wrapper with inline assembly.
inline void sfence() noexcept {
    // Intentionally left blank: core persistence primitive is not implemented in this scaffold.
}

}  // namespace pmem
