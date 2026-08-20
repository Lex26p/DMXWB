#include "dmxwb/app_info.hpp"
#include "dmxwb/dmx_snapshot.hpp"
#include "dmxwb/dmx_test_pattern.hpp"
#include "dmxwb/monotonic_clock.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>

namespace {

int failures = 0;

void expect_true(bool condition, std::string_view test_name) {
    if (condition) {
        std::cout << "[PASS] " << test_name << '\n';
        return;
    }

    ++failures;
    std::cerr << "[FAIL] " << test_name << '\n';
}

void expect_equal(std::string_view actual, std::string_view expected, std::string_view test_name) {
    expect_true(actual == expected, test_name);
}

class FakeMonotonicClock final : public dmxwb::MonotonicClock {
public:
    explicit FakeMonotonicClock(time_point value) noexcept : value_(value) {}

    [[nodiscard]] time_point now() const noexcept override {
        return value_;
    }

private:
    time_point value_;
};

std::shared_ptr<const dmxwb::DmxSnapshot> make_filled_snapshot(
    std::size_t slot_count,
    std::uint8_t value,
    dmxwb::DmxSnapshot::Generation generation) {
    const auto maybe_builder = dmxwb::DmxSnapshotBuilder::create(slot_count);
    if (!maybe_builder.has_value()) {
        return {};
    }

    auto builder = *maybe_builder;
    for (std::size_t channel = 1; channel <= slot_count; ++channel) {
        if (!builder.set_channel(channel, value)) {
            return {};
        }
    }
    return builder.build(generation);
}

void test_channel_boundaries() {
    auto maybe_builder = dmxwb::DmxSnapshotBuilder::create(dmxwb::kDmxMaxChannels);
    expect_true(maybe_builder.has_value(), "builder accepts 512 slots");
    if (!maybe_builder.has_value()) {
        return;
    }

    auto builder = *maybe_builder;
    expect_true(builder.set_channel(1, std::uint8_t{17}), "channel 1 accepted");
    expect_true(builder.set_channel(512, std::uint8_t{231}), "channel 512 accepted");
    expect_true(!builder.set_channel(0, std::uint8_t{1}), "channel 0 rejected");
    expect_true(!builder.set_channel(513, std::uint8_t{1}), "channel 513 rejected");

    const auto snapshot = builder.build(7);
    expect_true(snapshot->channel(1) == std::optional<std::uint8_t>{std::uint8_t{17}}, "channel 1 value preserved");
    expect_true(snapshot->channel(512) == std::optional<std::uint8_t>{std::uint8_t{231}}, "channel 512 value preserved");
    expect_true(!snapshot->channel(0).has_value(), "snapshot rejects channel 0");
    expect_true(!snapshot->channel(513).has_value(), "snapshot rejects channel 513");
    expect_true(!dmxwb::DmxSnapshotBuilder::create(513).has_value(), "slot_count above 512 rejected");
}

void test_slot_count_helpers() {
    expect_true(
        dmxwb::calculate_slot_count(1, 10, 4) == std::optional<std::size_t>{40},
        "slot_count 40 for start 1 / 10 RGBW items");
    expect_true(
        dmxwb::calculate_slot_count(21, 10, 4) == std::optional<std::size_t>{60},
        "slot_count 60 for start 21 / 10 RGBW items");
    expect_true(
        dmxwb::calculate_slot_count(1, 0, 4) == std::optional<std::size_t>{0},
        "zero items produce zero slots");
    expect_true(
        !dmxwb::calculate_slot_count(510, 1, 4).has_value(),
        "address range beyond 512 rejected");
    expect_true(
        !dmxwb::calculate_slot_count(1, 1, 0).has_value(),
        "zero channels per item rejected");
}

void test_start_code_and_payload_indexing() {
    auto maybe_builder = dmxwb::DmxSnapshotBuilder::create(2);
    expect_true(maybe_builder.has_value(), "two-slot builder created");
    if (!maybe_builder.has_value()) {
        return;
    }

    auto builder = *maybe_builder;
    expect_true(builder.set_channel(1, std::uint8_t{0x11}), "payload channel 1 set");
    expect_true(builder.set_channel(2, std::uint8_t{0x22}), "payload channel 2 set");
    const auto snapshot = builder.build(42);
    const auto frame = dmxwb::make_frame_view(*snapshot);

    expect_true(frame.start_code == 0x00, "DMX Start Code is separate 0x00");
    expect_true(frame.channels.size() == 2, "frame exposes exactly slot_count channels");
    expect_true(frame.channels[0] == 0x11, "frame payload index 0 is DMX channel 1");
    expect_true(frame.channels[1] == 0x22, "frame payload index 1 is DMX channel 2");
    expect_true(frame.generation == 42, "frame carries snapshot generation");
}

void test_immutable_publication() {
    dmxwb::DmxSnapshotPublisher publisher;

    const auto initial = publisher.load();
    expect_true(initial != nullptr, "publisher starts with a valid zero snapshot");
    expect_true(initial->slot_count() == 0, "initial snapshot has zero slots");
    expect_true(initial->generation() == 0, "initial snapshot generation is zero");

    const auto generation_one = make_filled_snapshot(8, 0x11, 1);
    const auto generation_two = make_filled_snapshot(8, 0x22, 2);
    expect_true(generation_one != nullptr && generation_two != nullptr, "publication fixtures built");
    if (!generation_one || !generation_two) {
        return;
    }

    expect_true(publisher.publish(generation_one), "generation 1 published");
    const auto held_generation_one = publisher.load();
    expect_true(held_generation_one->generation() == 1, "generation 1 observed whole");
    expect_true(held_generation_one->channel(1) == std::optional<std::uint8_t>{std::uint8_t{0x11}}, "generation 1 data observed");

    expect_true(publisher.publish(generation_two), "generation 2 published");
    const auto observed_generation_two = publisher.load();
    expect_true(observed_generation_two->generation() == 2, "generation 2 observed whole");
    expect_true(observed_generation_two->channel(8) == std::optional<std::uint8_t>{std::uint8_t{0x22}}, "generation 2 data observed");

    expect_true(held_generation_one->generation() == 1, "held generation remains immutable after publish");
    expect_true(held_generation_one->channel(8) == std::optional<std::uint8_t>{std::uint8_t{0x11}}, "held data remains immutable after publish");
    expect_true(!publisher.publish({}), "null snapshot publication rejected");
    expect_true(publisher.load()->generation() == 2, "rejected publication does not replace current snapshot");
}

void test_dmx_diagnostic_patterns() {
    expect_true(
        dmxwb::parse_dmx_test_pattern("red") == std::optional<dmxwb::DmxTestPattern>{dmxwb::DmxTestPattern::red},
        "diagnostic pattern parser accepts red");
    expect_true(
        dmxwb::parse_dmx_test_pattern("all-on") == std::optional<dmxwb::DmxTestPattern>{dmxwb::DmxTestPattern::all_on},
        "diagnostic pattern parser accepts all-on");
    expect_true(!dmxwb::parse_dmx_test_pattern("invalid").has_value(), "diagnostic pattern parser rejects invalid value");

    const auto red = dmxwb::make_dmx_test_snapshot(dmxwb::DmxTestPattern::red, 1, 10);
    expect_true(red != nullptr, "red diagnostic snapshot created at channel 1");
    if (red) {
        expect_true(red->slot_count() == 4, "channel-1 RGBW diagnostic uses four slots");
        expect_true(red->channel(1) == std::optional<std::uint8_t>{std::uint8_t{255}}, "red diagnostic sets R");
        expect_true(red->channel(2) == std::optional<std::uint8_t>{std::uint8_t{0}}, "red diagnostic clears G");
        expect_true(red->channel(3) == std::optional<std::uint8_t>{std::uint8_t{0}}, "red diagnostic clears B");
        expect_true(red->channel(4) == std::optional<std::uint8_t>{std::uint8_t{0}}, "red diagnostic clears W");
        expect_true(red->generation() == 10, "diagnostic snapshot preserves generation");
    }

    const auto white_at_21 = dmxwb::make_dmx_test_snapshot(dmxwb::DmxTestPattern::white, 21);
    expect_true(white_at_21 != nullptr, "white diagnostic snapshot created at channel 21");
    if (white_at_21) {
        expect_true(white_at_21->slot_count() == 24, "start channel 21 produces physical slot 24");
        expect_true(white_at_21->channel(20) == std::optional<std::uint8_t>{std::uint8_t{0}}, "preceding slot remains zero");
        expect_true(white_at_21->channel(21) == std::optional<std::uint8_t>{std::uint8_t{0}}, "white diagnostic clears R");
        expect_true(white_at_21->channel(22) == std::optional<std::uint8_t>{std::uint8_t{0}}, "white diagnostic clears G");
        expect_true(white_at_21->channel(23) == std::optional<std::uint8_t>{std::uint8_t{0}}, "white diagnostic clears B");
        expect_true(white_at_21->channel(24) == std::optional<std::uint8_t>{std::uint8_t{255}}, "white diagnostic sets W");
    }

    expect_true(
        !dmxwb::make_dmx_test_snapshot(dmxwb::DmxTestPattern::all_off, 0),
        "diagnostic rejects start channel 0");
    expect_true(
        !dmxwb::make_dmx_test_snapshot(dmxwb::DmxTestPattern::all_off, 510),
        "diagnostic rejects RGBW range beyond channel 512");
}

void test_clock_abstraction() {
    const auto expected = dmxwb::MonotonicClock::time_point{std::chrono::milliseconds{1234}};
    const FakeMonotonicClock clock{expected};
    expect_true(clock.now() == expected, "monotonic clock interface supports deterministic fake time");
}

}  // namespace

int main() {
    expect_equal(dmxwb::application_name(), "dmxwb", "application name");
    expect_equal(dmxwb::application_version(), "0.1.0", "application version");

    test_channel_boundaries();
    test_slot_count_helpers();
    test_start_code_and_payload_indexing();
    test_immutable_publication();
    test_dmx_diagnostic_patterns();
    test_clock_abstraction();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All tests passed\n";
    return 0;
}
