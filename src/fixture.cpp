#include "dmxwb/fixture.hpp"

#include <limits>
#include <utility>

namespace dmxwb {

Fixture::Fixture(Id id, std::string name)
    : id_(id), name_(std::move(name)) {}

Fixture::Id Fixture::id() const noexcept {
    return id_;
}

std::string_view Fixture::name() const noexcept {
    return name_;
}

void Fixture::set_name(std::string name) {
    name_ = std::move(name);
}

bool Fixture::requested_power() const noexcept {
    return requested_power_;
}

RgbwValues Fixture::saved_rgbw() const noexcept {
    return saved_;
}

std::uint8_t Fixture::brightness() const noexcept {
    return brightness_;
}

std::uint8_t Fixture::temperature() const noexcept {
    return temperature_;
}

RgbwValues Fixture::actual_rgbw() const noexcept {
    if (!requested_power_) {
        return {};
    }

    return RgbwValues{
        scale_channel(saved_.red, brightness_),
        scale_channel(saved_.green, brightness_),
        scale_channel(saved_.blue, brightness_),
        scale_channel(saved_.white, brightness_)};
}

bool Fixture::actual_power() const noexcept {
    const auto actual = actual_rgbw();
    return actual.red != 0 || actual.green != 0 || actual.blue != 0 || actual.white != 0;
}

void Fixture::set_power(bool on) noexcept {
    requested_power_ = on;
}

void Fixture::set_red(std::uint8_t value) noexcept {
    saved_.red = value;
    saved_.white = 0;
}

void Fixture::set_green(std::uint8_t value) noexcept {
    saved_.green = value;
    saved_.white = 0;
}

void Fixture::set_blue(std::uint8_t value) noexcept {
    saved_.blue = value;
    saved_.white = 0;
}

void Fixture::set_color(std::uint8_t red, std::uint8_t green, std::uint8_t blue) noexcept {
    saved_.red = red;
    saved_.green = green;
    saved_.blue = blue;
    saved_.white = 0;
}

bool Fixture::set_brightness(std::uint8_t percent) noexcept {
    if (percent > 100) {
        return false;
    }
    brightness_ = percent;
    return true;
}

bool Fixture::set_temperature(std::uint8_t percent) noexcept {
    if (percent > 100) {
        return false;
    }

    temperature_ = percent;
    saved_.red = 255;
    saved_.green = 255;
    saved_.blue = 255;
    saved_.white = temperature_to_white(percent);
    return true;
}

void Fixture::reset() noexcept {
    requested_power_ = true;
    saved_ = RgbwValues{255, 255, 255, 255};
    brightness_ = 100;
    temperature_ = 100;
}

std::uint8_t Fixture::scale_channel(std::uint8_t value, std::uint8_t percent) noexcept {
    const auto scaled =
        (static_cast<std::uint32_t>(value) * static_cast<std::uint32_t>(percent)) / 100U;
    return static_cast<std::uint8_t>(scaled);
}

std::uint8_t Fixture::temperature_to_white(std::uint8_t percent) noexcept {
    const auto scaled =
        (static_cast<std::uint32_t>(percent) * 255U + 50U) / 100U;
    return static_cast<std::uint8_t>(scaled);
}

std::size_t FixtureCollection::fixture_count() const noexcept {
    return fixtures_.size();
}

std::size_t FixtureCollection::start_address() const noexcept {
    return start_address_;
}

Fixture* FixtureCollection::fixture_at(std::size_t zero_based_index) noexcept {
    if (zero_based_index >= fixtures_.size()) {
        return nullptr;
    }
    return &fixtures_[zero_based_index];
}

const Fixture* FixtureCollection::fixture_at(std::size_t zero_based_index) const noexcept {
    if (zero_based_index >= fixtures_.size()) {
        return nullptr;
    }
    return &fixtures_[zero_based_index];
}

bool FixtureCollection::set_fixture_count(std::size_t count) {
    return reconfigure(count, start_address_);
}

bool FixtureCollection::set_start_address(std::size_t start_address) noexcept {
    if (!is_valid_configuration(fixtures_.size(), start_address)) {
        return false;
    }
    start_address_ = start_address;
    return true;
}

bool FixtureCollection::reconfigure(std::size_t count, std::size_t start_address) {
    if (!is_valid_configuration(count, start_address)) {
        return false;
    }

    if (count < fixtures_.size()) {
        fixtures_.erase(fixtures_.begin() + static_cast<std::ptrdiff_t>(count), fixtures_.end());
    } else {
        while (fixtures_.size() < count) {
            if (next_fixture_id_ == std::numeric_limits<Fixture::Id>::max()) {
                return false;
            }
            const auto position = fixtures_.size() + 1;
            fixtures_.emplace_back(next_fixture_id_, default_fixture_name(position));
            ++next_fixture_id_;
        }
    }

    start_address_ = start_address;
    return true;
}

std::optional<std::size_t> FixtureCollection::fixture_start_address(
    std::size_t zero_based_index) const noexcept {
    if (zero_based_index >= fixtures_.size()) {
        return std::nullopt;
    }

    return start_address_ + zero_based_index * kFixtureChannels;
}

std::optional<std::size_t> FixtureCollection::physical_slot_count() const noexcept {
    return calculate_slot_count(start_address_, fixtures_.size(), kFixtureChannels);
}

std::shared_ptr<const DmxSnapshot> FixtureCollection::build_snapshot(
    DmxSnapshot::Generation generation) const {
    const auto maybe_slot_count = physical_slot_count();
    if (!maybe_slot_count.has_value()) {
        return {};
    }

    const auto maybe_builder = DmxSnapshotBuilder::create(*maybe_slot_count);
    if (!maybe_builder.has_value()) {
        return {};
    }

    auto builder = *maybe_builder;
    for (std::size_t index = 0; index < fixtures_.size(); ++index) {
        const auto maybe_start = fixture_start_address(index);
        if (!maybe_start.has_value()) {
            return {};
        }

        const auto actual = fixtures_[index].actual_rgbw();
        if (!builder.set_channel(*maybe_start, actual.red) ||
            !builder.set_channel(*maybe_start + 1, actual.green) ||
            !builder.set_channel(*maybe_start + 2, actual.blue) ||
            !builder.set_channel(*maybe_start + 3, actual.white)) {
            return {};
        }
    }

    return builder.build(generation);
}

bool FixtureCollection::is_valid_configuration(
    std::size_t count,
    std::size_t start_address) noexcept {
    if (count == 0) {
        return start_address >= 1 && start_address <= kDmxPhysicalMaxSlots;
    }
    return calculate_slot_count(start_address, count, kFixtureChannels).has_value();
}

std::string FixtureCollection::default_fixture_name(std::size_t one_based_position) {
    return std::string{"Светильник "} + std::to_string(one_based_position);
}

}  // namespace dmxwb
