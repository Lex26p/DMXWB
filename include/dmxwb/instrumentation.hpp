#pragma once

#include <cstdint>

namespace dmxwb {

// Engineering instrumentation is an execution policy, not a second runtime
// implementation. Production keeps the same algorithms and factual state but
// does not accumulate test/acceptance counters.
enum class InstrumentationMode : std::uint8_t {
    production,
    engineering,
};

[[nodiscard]] constexpr bool engineering_instrumentation_enabled(
    InstrumentationMode mode) noexcept {
    return mode == InstrumentationMode::engineering;
}

}  // namespace dmxwb
