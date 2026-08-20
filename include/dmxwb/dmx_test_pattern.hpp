#pragma once

#include "dmxwb/dmx_snapshot.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace dmxwb {

enum class DmxTestPattern {
    all_off,
    red,
    green,
    blue,
    white,
    all_on,
};

[[nodiscard]] std::optional<DmxTestPattern> parse_dmx_test_pattern(std::string_view value) noexcept;
[[nodiscard]] std::string_view dmx_test_pattern_name(DmxTestPattern pattern) noexcept;

[[nodiscard]] std::shared_ptr<const DmxSnapshot> make_dmx_test_snapshot(
    DmxTestPattern pattern,
    std::size_t start_channel,
    DmxSnapshot::Generation generation = 1);

}  // namespace dmxwb
