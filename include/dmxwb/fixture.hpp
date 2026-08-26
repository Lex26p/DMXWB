#pragma once

#include "dmxwb/dmx_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dmxwb {

inline constexpr std::size_t kFixtureChannels = 4;
inline constexpr std::uint8_t kFixtureDefaultChannelValue = 255;
inline constexpr std::uint8_t kFixtureDefaultBrightness = 100;
inline constexpr std::uint8_t kFixtureDefaultTemperature = 100;

struct RgbwValues final {
    std::uint8_t red{0};
    std::uint8_t green{0};
    std::uint8_t blue{0};
    std::uint8_t white{0};

    [[nodiscard]] friend constexpr bool operator==(const RgbwValues&, const RgbwValues&) noexcept = default;
};

class Fixture final {
public:
    using Id = std::uint64_t;

    Fixture(Id id, std::string name);

    [[nodiscard]] Id id() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    void set_name(std::string name);

    [[nodiscard]] bool requested_power() const noexcept;
    [[nodiscard]] RgbwValues saved_rgbw() const noexcept;
    [[nodiscard]] std::uint8_t brightness() const noexcept;
    [[nodiscard]] std::uint8_t temperature() const noexcept;

    [[nodiscard]] RgbwValues actual_rgbw() const noexcept;
    [[nodiscard]] bool actual_power() const noexcept;

    void set_power(bool on) noexcept;
    void set_red(std::uint8_t value) noexcept;
    void set_green(std::uint8_t value) noexcept;
    void set_blue(std::uint8_t value) noexcept;
    void set_color(std::uint8_t red, std::uint8_t green, std::uint8_t blue) noexcept;
    [[nodiscard]] bool set_brightness(std::uint8_t percent) noexcept;
    [[nodiscard]] bool set_temperature(std::uint8_t percent) noexcept;
    [[nodiscard]] bool restore_state(
        bool requested_power,
        RgbwValues saved_rgbw,
        std::uint8_t brightness,
        std::uint8_t temperature) noexcept;
    void reset() noexcept;

private:
    [[nodiscard]] static std::uint8_t scale_channel(std::uint8_t value, std::uint8_t percent) noexcept;
    [[nodiscard]] static std::uint8_t temperature_to_white(std::uint8_t percent) noexcept;

    Id id_{0};
    std::string name_;
    bool requested_power_{false};
    RgbwValues saved_{
        kFixtureDefaultChannelValue,
        kFixtureDefaultChannelValue,
        kFixtureDefaultChannelValue,
        kFixtureDefaultChannelValue};
    std::uint8_t brightness_{kFixtureDefaultBrightness};
    std::uint8_t temperature_{kFixtureDefaultTemperature};
};

class FixtureCollection final {
public:
    FixtureCollection() = default;

    [[nodiscard]] std::size_t fixture_count() const noexcept;
    [[nodiscard]] std::size_t start_address() const noexcept;
    [[nodiscard]] Fixture::Id next_fixture_id() const noexcept;

    [[nodiscard]] Fixture* fixture_at(std::size_t zero_based_index) noexcept;
    [[nodiscard]] const Fixture* fixture_at(std::size_t zero_based_index) const noexcept;

    [[nodiscard]] bool set_fixture_count(std::size_t count);
    [[nodiscard]] bool set_start_address(std::size_t start_address) noexcept;
    [[nodiscard]] bool reconfigure(std::size_t count, std::size_t start_address);
    [[nodiscard]] bool restore(
        std::vector<Fixture> fixtures,
        std::size_t start_address,
        Fixture::Id next_fixture_id);

    [[nodiscard]] std::optional<std::size_t> fixture_start_address(
        std::size_t zero_based_index) const noexcept;
    [[nodiscard]] std::optional<std::size_t> physical_slot_count() const noexcept;

    [[nodiscard]] std::shared_ptr<const DmxSnapshot> build_snapshot(
        DmxSnapshot::Generation generation) const;

private:
    [[nodiscard]] static bool is_valid_configuration(std::size_t count, std::size_t start_address) noexcept;
    [[nodiscard]] static std::string default_fixture_name(std::size_t one_based_position);

    std::vector<Fixture> fixtures_;
    std::size_t start_address_{1};
    Fixture::Id next_fixture_id_{1};
};

}  // namespace dmxwb
