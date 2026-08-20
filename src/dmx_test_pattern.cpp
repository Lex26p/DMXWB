#include "dmxwb/dmx_test_pattern.hpp"

#include <array>
#include <cstdint>

namespace dmxwb {
namespace {

using Rgbw = std::array<std::uint8_t, 4>;

[[nodiscard]] constexpr Rgbw pattern_values(DmxTestPattern pattern) noexcept {
    switch (pattern) {
        case DmxTestPattern::all_off:
            return Rgbw{0, 0, 0, 0};
        case DmxTestPattern::red:
            return Rgbw{255, 0, 0, 0};
        case DmxTestPattern::green:
            return Rgbw{0, 255, 0, 0};
        case DmxTestPattern::blue:
            return Rgbw{0, 0, 255, 0};
        case DmxTestPattern::white:
            return Rgbw{0, 0, 0, 255};
        case DmxTestPattern::all_on:
            return Rgbw{255, 255, 255, 255};
    }
    return Rgbw{};
}

}  // namespace

std::optional<DmxTestPattern> parse_dmx_test_pattern(std::string_view value) noexcept {
    if (value == "all-off") {
        return DmxTestPattern::all_off;
    }
    if (value == "red") {
        return DmxTestPattern::red;
    }
    if (value == "green") {
        return DmxTestPattern::green;
    }
    if (value == "blue") {
        return DmxTestPattern::blue;
    }
    if (value == "white") {
        return DmxTestPattern::white;
    }
    if (value == "all-on") {
        return DmxTestPattern::all_on;
    }
    return std::nullopt;
}

std::string_view dmx_test_pattern_name(DmxTestPattern pattern) noexcept {
    switch (pattern) {
        case DmxTestPattern::all_off:
            return "all-off";
        case DmxTestPattern::red:
            return "red";
        case DmxTestPattern::green:
            return "green";
        case DmxTestPattern::blue:
            return "blue";
        case DmxTestPattern::white:
            return "white";
        case DmxTestPattern::all_on:
            return "all-on";
    }
    return "unknown";
}

std::shared_ptr<const DmxSnapshot> make_dmx_test_snapshot(
    DmxTestPattern pattern,
    std::size_t start_channel,
    DmxSnapshot::Generation generation) {
    constexpr std::size_t kRgbwWidth = 4;
    const auto slot_count = calculate_slot_count(start_channel, 1, kRgbwWidth);
    if (!slot_count.has_value()) {
        return {};
    }

    auto maybe_builder = DmxSnapshotBuilder::create(*slot_count);
    if (!maybe_builder.has_value()) {
        return {};
    }

    auto builder = *maybe_builder;
    const auto values = pattern_values(pattern);
    for (std::size_t offset = 0; offset < values.size(); ++offset) {
        if (!builder.set_channel(start_channel + offset, values[offset])) {
            return {};
        }
    }

    return builder.build(generation);
}

}  // namespace dmxwb
