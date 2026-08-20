#include "dmxwb/dmx_snapshot.hpp"

#include <algorithm>
#include <utility>

namespace dmxwb {

std::optional<std::size_t> calculate_slot_count(
    std::size_t start_channel,
    std::size_t item_count,
    std::size_t channels_per_item) noexcept {
    if (channels_per_item == 0) {
        return std::nullopt;
    }

    if (item_count == 0) {
        return std::size_t{0};
    }

    if (!is_valid_dmx_channel(start_channel)) {
        return std::nullopt;
    }

    const auto available_channels = kDmxMaxChannels - (start_channel - 1);
    if (item_count > available_channels / channels_per_item) {
        return std::nullopt;
    }

    return (start_channel - 1) + (item_count * channels_per_item);
}

DmxSnapshot::DmxSnapshot(ChannelData channels, std::size_t slot_count, Generation generation) noexcept
    : channels_(std::move(channels)), slot_count_(slot_count), generation_(generation) {}

std::size_t DmxSnapshot::slot_count() const noexcept {
    return slot_count_;
}

DmxSnapshot::Generation DmxSnapshot::generation() const noexcept {
    return generation_;
}

std::optional<std::uint8_t> DmxSnapshot::channel(std::size_t one_based_channel) const noexcept {
    const auto index = dmx_channel_to_index(one_based_channel);
    if (!index.has_value()) {
        return std::nullopt;
    }
    return channels_[*index];
}

std::span<const std::uint8_t> DmxSnapshot::active_channels() const noexcept {
    return std::span<const std::uint8_t>{channels_.data(), slot_count_};
}

std::optional<DmxSnapshotBuilder> DmxSnapshotBuilder::create(std::size_t slot_count) noexcept {
    if (slot_count > kDmxMaxChannels) {
        return std::nullopt;
    }
    return DmxSnapshotBuilder{slot_count};
}

DmxSnapshotBuilder::DmxSnapshotBuilder(std::size_t slot_count) noexcept : slot_count_(slot_count) {}

bool DmxSnapshotBuilder::set_channel(std::size_t one_based_channel, std::uint8_t value) noexcept {
    const auto index = dmx_channel_to_index(one_based_channel);
    if (!index.has_value() || one_based_channel > slot_count_) {
        return false;
    }

    channels_[*index] = value;
    return true;
}

std::shared_ptr<const DmxSnapshot> DmxSnapshotBuilder::build(DmxSnapshot::Generation generation) const {
    return std::shared_ptr<const DmxSnapshot>{new DmxSnapshot(channels_, slot_count_, generation)};
}

std::size_t DmxSnapshotBuilder::slot_count() const noexcept {
    return slot_count_;
}

DmxFrameView make_frame_view(const DmxSnapshot& snapshot) noexcept {
    return DmxFrameView{kDmxStartCode, snapshot.active_channels(), snapshot.generation()};
}

DmxSnapshotPublisher::DmxSnapshotPublisher()
    : current_(std::make_shared<const DmxSnapshot>()) {}

bool DmxSnapshotPublisher::publish(std::shared_ptr<const DmxSnapshot> snapshot) noexcept {
    if (!snapshot) {
        return false;
    }
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    current_.store(std::move(snapshot), std::memory_order_release);
#else
    std::atomic_store_explicit(&current_, std::move(snapshot), std::memory_order_release);
#endif
    return true;
}

std::shared_ptr<const DmxSnapshot> DmxSnapshotPublisher::load() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return current_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
#endif
}

}  // namespace dmxwb
