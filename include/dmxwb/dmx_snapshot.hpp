#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace dmxwb {

inline constexpr std::size_t kDmxMaxChannels = 512;
inline constexpr std::uint8_t kDmxStartCode = 0x00;

[[nodiscard]] constexpr bool is_valid_dmx_channel(std::size_t channel) noexcept {
    return channel >= 1 && channel <= kDmxMaxChannels;
}

[[nodiscard]] constexpr std::optional<std::size_t> dmx_channel_to_index(std::size_t channel) noexcept {
    if (!is_valid_dmx_channel(channel)) {
        return std::nullopt;
    }
    return channel - 1;
}

[[nodiscard]] std::optional<std::size_t> calculate_slot_count(
    std::size_t start_channel,
    std::size_t item_count,
    std::size_t channels_per_item) noexcept;

class DmxSnapshot final {
public:
    using ChannelData = std::array<std::uint8_t, kDmxMaxChannels>;
    using Generation = std::uint64_t;

    DmxSnapshot() = default;

    [[nodiscard]] std::size_t slot_count() const noexcept;
    [[nodiscard]] Generation generation() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> channel(std::size_t one_based_channel) const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> active_channels() const noexcept;

private:
    friend class DmxSnapshotBuilder;

    DmxSnapshot(ChannelData channels, std::size_t slot_count, Generation generation) noexcept;

    ChannelData channels_{};
    std::size_t slot_count_{0};
    Generation generation_{0};
};

class DmxSnapshotBuilder final {
public:
    [[nodiscard]] static std::optional<DmxSnapshotBuilder> create(std::size_t slot_count) noexcept;

    [[nodiscard]] bool set_channel(std::size_t one_based_channel, std::uint8_t value) noexcept;
    [[nodiscard]] std::shared_ptr<const DmxSnapshot> build(DmxSnapshot::Generation generation) const;

    [[nodiscard]] std::size_t slot_count() const noexcept;

private:
    explicit DmxSnapshotBuilder(std::size_t slot_count) noexcept;

    DmxSnapshot::ChannelData channels_{};
    std::size_t slot_count_{0};
};

struct DmxFrameView final {
    std::uint8_t start_code{kDmxStartCode};
    std::span<const std::uint8_t> channels{};
    DmxSnapshot::Generation generation{0};
};

[[nodiscard]] DmxFrameView make_frame_view(const DmxSnapshot& snapshot) noexcept;

class DmxSnapshotPublisher final {
public:
    DmxSnapshotPublisher();

    [[nodiscard]] bool publish(std::shared_ptr<const DmxSnapshot> snapshot) noexcept;
    [[nodiscard]] std::shared_ptr<const DmxSnapshot> load() const noexcept;

private:
    std::atomic<std::shared_ptr<const DmxSnapshot>> current_;
};

}  // namespace dmxwb
