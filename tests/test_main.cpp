#include "dmxwb/app_info.hpp"
#include "dmxwb/dmx_output.hpp"
#include "dmxwb/fixture.hpp"
#include "dmxwb/dmx_snapshot.hpp"
#include "dmxwb/dmx_test_pattern.hpp"
#include "dmxwb/dmx_transport.hpp"
#include "dmxwb/monotonic_clock.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
    explicit FakeMonotonicClock(time_point value = {}) noexcept : value_(value) {}

    [[nodiscard]] time_point now() const noexcept override {
        return value_;
    }

    void sleep_until(time_point deadline) override {
        sleep_deadlines_.push_back(deadline);
        if (deadline > value_) {
            value_ = deadline;
        }
    }

    void advance(duration amount) noexcept {
        value_ += amount;
    }

    [[nodiscard]] const std::vector<time_point>& sleep_deadlines() const noexcept {
        return sleep_deadlines_;
    }

private:
    time_point value_{};
    std::vector<time_point> sleep_deadlines_;
};

class FakeDmxTransport final : public dmxwb::DmxTransportInterface {
public:
    explicit FakeDmxTransport(FakeMonotonicClock& clock) : clock_(clock) {}

    [[nodiscard]] bool open() override {
        ++open_calls_;
        bool result = true;
        if (open_result_index_ < open_results_.size()) {
            result = open_results_[open_result_index_];
            ++open_result_index_;
        }
        open_ = result;
        if (!result) {
            last_error_ = "simulated open failure";
        }
        return result;
    }

    void close() noexcept override {
        ++close_calls_;
        open_ = false;
    }

    [[nodiscard]] bool is_open() const noexcept override {
        return open_;
    }

    [[nodiscard]] std::string_view port() const noexcept override {
        return "/dev/fake-dmx";
    }

    [[nodiscard]] std::string_view last_error() const noexcept override {
        return last_error_;
    }

    [[nodiscard]] bool send_frame(const dmxwb::DmxFrameView& frame) override {
        send_start_times_.push_back(clock_.now());
        generations_.push_back(frame.generation);
        first_channels_.push_back(frame.channels.empty() ? std::uint8_t{0} : frame.channels.front());

        const auto call_index = send_calls_;
        ++send_calls_;
        if (on_send_) {
            on_send_(call_index);
        }
        clock_.advance(send_duration_);

        bool result = true;
        if (send_result_index_ < send_results_.size()) {
            result = send_results_[send_result_index_];
            ++send_result_index_;
        }
        if (!result) {
            last_error_ = "simulated send failure";
        }
        return result;
    }

    void set_open_results(std::vector<bool> results) {
        open_results_ = std::move(results);
        open_result_index_ = 0;
    }

    void set_send_results(std::vector<bool> results) {
        send_results_ = std::move(results);
        send_result_index_ = 0;
    }

    void set_send_duration(dmxwb::MonotonicClock::duration duration) noexcept {
        send_duration_ = duration;
    }

    void set_on_send(std::function<void(std::size_t)> callback) {
        on_send_ = std::move(callback);
    }

    [[nodiscard]] std::size_t open_calls() const noexcept {
        return open_calls_;
    }

    [[nodiscard]] std::size_t close_calls() const noexcept {
        return close_calls_;
    }

    [[nodiscard]] const std::vector<dmxwb::MonotonicClock::time_point>& send_start_times() const noexcept {
        return send_start_times_;
    }

    [[nodiscard]] const std::vector<dmxwb::DmxSnapshot::Generation>& generations() const noexcept {
        return generations_;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& first_channels() const noexcept {
        return first_channels_;
    }

private:
    FakeMonotonicClock& clock_;
    bool open_{false};
    std::string last_error_;
    std::vector<bool> open_results_;
    std::vector<bool> send_results_;
    std::size_t open_result_index_{0};
    std::size_t send_result_index_{0};
    std::size_t open_calls_{0};
    std::size_t close_calls_{0};
    std::size_t send_calls_{0};
    dmxwb::MonotonicClock::duration send_duration_{};
    std::function<void(std::size_t)> on_send_;
    std::vector<dmxwb::MonotonicClock::time_point> send_start_times_;
    std::vector<dmxwb::DmxSnapshot::Generation> generations_;
    std::vector<std::uint8_t> first_channels_;
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

bool drive_until_frames(
    dmxwb::DmxOutputLoop& loop,
    FakeMonotonicClock& clock,
    std::uint64_t frame_count,
    std::size_t max_steps = 1000) {
    for (std::size_t step_number = 0; step_number < max_steps; ++step_number) {
        if (loop.diagnostics().frames_sent >= frame_count) {
            return true;
        }
        const auto result = loop.step();
        if (result.kind == dmxwb::DmxOutputStepKind::wait_until) {
            clock.sleep_until(result.wake_at);
        }
    }
    return loop.diagnostics().frames_sent >= frame_count;
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
        dmxwb::calculate_slot_count(297, 1, 4) == std::optional<std::size_t>{300},
        "RGBW fixture ending at physical slot 300 accepted");
    expect_true(
        !dmxwb::calculate_slot_count(298, 1, 4).has_value(),
        "RGBW fixture crossing physical slot 300 rejected");
    expect_true(
        !dmxwb::calculate_slot_count(510, 1, 4).has_value(),
        "start address outside physical 300-slot profile rejected");
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
    FakeMonotonicClock clock{expected};
    expect_true(clock.now() == expected, "monotonic clock interface supports deterministic fake time");

    const auto deadline = expected + std::chrono::milliseconds{50};
    clock.sleep_until(deadline);
    expect_true(clock.now() == deadline, "fake monotonic clock advances to absolute deadline");
}

void test_fixed_physical_output_profile() {
    expect_true(dmxwb::kDmxMaxChannels == 512, "DMX/Art-Net core retains 512-channel data capacity");
    expect_true(dmxwb::kDmxPhysicalMaxSlots == 300, "physical DMXWB output is limited to 300 slots");
    expect_true(dmxwb::kDmxOutputRefreshHz == 44, "physical DMXWB output cadence is fixed at 44 Hz");

    const auto snapshot_300 = make_filled_snapshot(300, 0x22, 21);
    const auto snapshot_301 = make_filled_snapshot(301, 0x33, 22);
    const auto snapshot_512 = make_filled_snapshot(512, 0x44, 23);
    expect_true(snapshot_300 != nullptr && snapshot_301 != nullptr && snapshot_512 != nullptr,
                "core snapshots can still represent 300, 301 and 512 channels");
    if (!snapshot_300 || !snapshot_301 || !snapshot_512) {
        return;
    }

    dmxwb::DmxOutputMailbox mailbox;
    expect_true(mailbox.publish(*snapshot_300), "physical mailbox accepts exactly 300 slots");
    expect_true(!mailbox.publish(*snapshot_301), "physical mailbox rejects slot 301 and above");
    expect_true(!mailbox.publish(*snapshot_512), "physical mailbox rejects a 512-slot snapshot");
    expect_true(mailbox.published_slot_count() == 300, "rejected oversized snapshot does not replace physical frame");

    const auto wire_time = dmxwb::minimum_dmx_frame_time(300);
    const auto period = std::chrono::nanoseconds{1'000'000'000LL / 44LL};
    expect_true(wire_time < period, "300-slot DMX wire time fits inside the fixed 44 Hz period before OS overhead");
}

void test_preallocated_mailbox_frame_boundary() {
    dmxwb::DmxOutputMailbox mailbox;
    const auto generation_one = make_filled_snapshot(4, 0x11, 1);
    const auto generation_two = make_filled_snapshot(4, 0x22, 2);
    expect_true(generation_one != nullptr && generation_two != nullptr, "mailbox snapshots built");
    if (!generation_one || !generation_two) {
        return;
    }

    expect_true(mailbox.publish(*generation_one), "mailbox publishes generation 1");
    const auto held = mailbox.acquire();
    expect_true(held.generation == 1, "mailbox front frame holds generation 1");
    expect_true(held.channels[0] == 0x11, "mailbox front frame holds generation 1 data");

    expect_true(mailbox.publish(*generation_two), "mailbox publishes generation 2 while generation 1 is active");
    expect_true(held.generation == 1, "active frame view cannot change mid-frame before next acquire");
    expect_true(held.channels[0] == 0x11, "active frame data cannot change mid-frame before next acquire");

    const auto latest = mailbox.acquire();
    expect_true(latest.generation == 2, "next frame acquire sees generation 2");
    expect_true(latest.channels[0] == 0x22, "next frame acquire sees generation 2 data");
}

void test_mailbox_concurrent_whole_frames() {
    dmxwb::DmxOutputMailbox mailbox;
    std::atomic<bool> writer_done{false};
    std::atomic<bool> torn_frame{false};

    std::thread writer{[&] {
        for (std::uint64_t generation = 1; generation <= 5000; ++generation) {
            const auto value = static_cast<std::uint8_t>(generation % 251U);
            const auto snapshot = make_filled_snapshot(8, value, generation);
            if (!snapshot || !mailbox.publish(*snapshot)) {
                torn_frame.store(true, std::memory_order_release);
                break;
            }
        }
        writer_done.store(true, std::memory_order_release);
    }};

    while (!writer_done.load(std::memory_order_acquire)) {
        const auto frame = mailbox.acquire();
        if (frame.generation == 0) {
            continue;
        }
        const auto expected = static_cast<std::uint8_t>(frame.generation % 251U);
        for (const auto value : frame.channels) {
            if (value != expected) {
                torn_frame.store(true, std::memory_order_release);
                break;
            }
        }
    }

    writer.join();
    const auto final_frame = mailbox.acquire();
    expect_true(!torn_frame.load(std::memory_order_acquire), "concurrent mailbox reader never observes torn frame data");
    expect_true(final_frame.generation == 5000, "concurrent mailbox reader can acquire final published generation");
}

void test_absolute_frame_schedule_no_drift() {
    FakeMonotonicClock clock;
    FakeDmxTransport transport{clock};
    transport.set_send_duration(std::chrono::milliseconds{2});

    dmxwb::DmxOutputMailbox mailbox;
    const auto snapshot = make_filled_snapshot(4, 0x33, 7);
    expect_true(snapshot != nullptr, "absolute schedule snapshot built");
    if (!snapshot) {
        return;
    }
    expect_true(mailbox.publish(*snapshot), "absolute schedule snapshot published");

    dmxwb::DmxOutputLoop loop{transport, mailbox, clock};
    expect_true(drive_until_frames(loop, clock, 3), "absolute scheduler produced three frames");

    const auto& starts = transport.send_start_times();
    expect_true(starts.size() >= 3, "absolute scheduler recorded three frame starts");
    if (starts.size() >= 3) {
        constexpr auto period_ns = 1'000'000'000LL / 44LL;
        expect_true(starts[0] == dmxwb::MonotonicClock::time_point{}, "first frame starts at T0");
        expect_true(
            starts[1] == dmxwb::MonotonicClock::time_point{std::chrono::nanoseconds{period_ns}},
            "second frame starts at T0 + fixed 44 Hz period");
        expect_true(
            starts[2] == dmxwb::MonotonicClock::time_point{std::chrono::nanoseconds{period_ns * 2}},
            "third frame stays on the fixed absolute 44 Hz grid without drift");
    }
    expect_true(loop.diagnostics().active_refresh_hz == 44, "diagnostics report fixed 44 Hz output");
    expect_true(loop.diagnostics().missed_deadlines == 0, "normal send duration does not miss deadlines");
    loop.shutdown();
}

void test_snapshot_switch_only_between_frames() {
    FakeMonotonicClock clock;
    FakeDmxTransport transport{clock};
    transport.set_send_duration(std::chrono::milliseconds{1});

    dmxwb::DmxOutputMailbox mailbox;
    const auto generation_one = make_filled_snapshot(4, 0x11, 1);
    const auto generation_two = make_filled_snapshot(4, 0x22, 2);
    expect_true(generation_one != nullptr && generation_two != nullptr, "frame-boundary snapshots built");
    if (!generation_one || !generation_two) {
        return;
    }
    expect_true(mailbox.publish(*generation_one), "frame-boundary generation 1 published");

    transport.set_on_send([&](std::size_t call_index) {
        if (call_index == 0) {
            (void)mailbox.publish(*generation_two);
        }
    });

    dmxwb::DmxOutputLoop loop{transport, mailbox, clock};
    expect_true(drive_until_frames(loop, clock, 2), "frame-boundary scheduler produced two frames");

    const auto& generations = transport.generations();
    const auto& values = transport.first_channels();
    expect_true(generations.size() >= 2 && values.size() >= 2, "frame-boundary transport recorded two frames");
    if (generations.size() >= 2 && values.size() >= 2) {
        expect_true(generations[0] == 1 && values[0] == 0x11, "first frame remains entirely generation 1");
        expect_true(generations[1] == 2 && values[1] == 0x22, "next frame switches entirely to generation 2");
    }
    loop.shutdown();
}

void test_serial_failure_reopen_recovery() {
    FakeMonotonicClock clock;
    FakeDmxTransport transport{clock};
    transport.set_open_results({true, true});
    transport.set_send_results({true, false, true});
    transport.set_send_duration(std::chrono::milliseconds{1});

    dmxwb::DmxOutputMailbox mailbox;
    const auto snapshot = make_filled_snapshot(4, 0x44, 9);
    expect_true(snapshot != nullptr, "recovery snapshot built");
    if (!snapshot) {
        return;
    }
    expect_true(mailbox.publish(*snapshot), "recovery snapshot published");

    dmxwb::DmxOutputLoop loop{
        transport,
        mailbox,
        clock,
        std::chrono::milliseconds{250}};

    expect_true(drive_until_frames(loop, clock, 2), "output resumes after simulated serial send failure");
    const auto diagnostics = loop.diagnostics();
    expect_true(diagnostics.send_failures == 1, "serial send failure counted");
    expect_true(diagnostics.reopen_attempts == 1, "serial reopen attempted after failure");
    expect_true(diagnostics.recoveries == 1, "serial recovery counted after successful reopen");
    expect_true(diagnostics.frames_sent == 2, "frames continue after recovery");
    expect_true(transport.open_calls() == 2, "transport opened initially and once after failure");
    expect_true(transport.close_calls() >= 1, "failed transport is closed before reopen");
    loop.shutdown();
}

void test_missed_deadline_is_counted_and_grid_recovers() {
    FakeMonotonicClock clock;
    FakeDmxTransport transport{clock};
    transport.set_send_duration(std::chrono::milliseconds{30});

    dmxwb::DmxOutputMailbox mailbox;
    const auto snapshot = make_filled_snapshot(4, 0x66, 13);
    expect_true(snapshot != nullptr, "deadline snapshot built");
    if (!snapshot) {
        return;
    }
    expect_true(mailbox.publish(*snapshot), "deadline snapshot published");

    dmxwb::DmxOutputLoop loop{transport, mailbox, clock};
    expect_true(drive_until_frames(loop, clock, 2), "deadline test produced two frames");
    expect_true(loop.diagnostics().missed_deadlines >= 1, "frame longer than fixed 44 Hz period increments missed deadline counter");

    const auto& starts = transport.send_start_times();
    expect_true(starts.size() >= 2, "deadline test recorded two starts");
    if (starts.size() >= 2) {
        constexpr auto period_ns = 1'000'000'000LL / 44LL;
        expect_true(
            starts[1] == dmxwb::MonotonicClock::time_point{std::chrono::nanoseconds{period_ns * 2}},
            "scheduler skips one missed fixed-rate boundary and returns to the absolute grid");
    }
    loop.shutdown();
}

void test_production_dmx_output_keeps_factual_state_without_counters() {
    FakeMonotonicClock clock;
    FakeDmxTransport transport{clock};
    dmxwb::DmxOutputMailbox mailbox;
    const auto snapshot = make_filled_snapshot(4, 91, 700);
    expect_true(snapshot != nullptr, "production-mode DMX snapshot created");
    if (!snapshot) {
        return;
    }
    expect_true(mailbox.publish(*snapshot), "production-mode DMX snapshot published");

    dmxwb::DmxOutputLoop loop{
        transport,
        mailbox,
        clock,
        std::chrono::milliseconds{250},
        dmxwb::InstrumentationMode::production};
    const auto step = loop.step();
    expect_true(step.kind == dmxwb::DmxOutputStepKind::frame_sent,
        "production mode still sends the physical DMX frame");
    expect_true(transport.open_calls() == 1 && transport.generations().size() == 1,
        "production mode still opens transport and sends exactly one frame");

    const auto diagnostics = loop.diagnostics();
    expect_true(diagnostics.frames_sent == 0 &&
                    diagnostics.open_attempts == 0 &&
                    diagnostics.reopen_attempts == 0 &&
                    diagnostics.open_failures == 0 &&
                    diagnostics.send_failures == 0 &&
                    diagnostics.recoveries == 0 &&
                    diagnostics.missed_deadlines == 0 &&
                    diagnostics.max_send_duration.count() == 0 &&
                    diagnostics.max_transport_overhead.count() == 0,
        "production DMX hot path does not accumulate engineering counters");
    expect_true(diagnostics.active_generation == 700 && diagnostics.serial_open,
        "production DMX diagnostics retain current factual state");
    loop.shutdown();
}


void test_fixture_initial_state_and_identity() {
    dmxwb::FixtureCollection fixtures;
    expect_true(fixtures.reconfigure(2, 1), "fixture collection creates two fixtures");

    const auto* first = fixtures.fixture_at(0);
    const auto* second = fixtures.fixture_at(1);
    expect_true(first != nullptr && second != nullptr, "created fixtures are addressable");
    if (!first || !second) {
        return;
    }

    expect_true(first->id() == 1 && second->id() == 2, "fixture IDs start monotonically at 1");
    expect_true(first->name() == "Светильник 1", "first fixture gets default Russian name");
    expect_true(second->name() == "Светильник 2", "second fixture gets default Russian name");
    expect_true(!first->requested_power(), "new fixture starts requested OFF");
    expect_true(
        first->saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 255},
        "new fixture stores full RGBW values");
    expect_true(first->brightness() == 100, "new fixture brightness defaults to 100");
    expect_true(first->temperature() == 100, "new fixture temperature defaults to 100");
    expect_true(first->actual_rgbw() == dmxwb::RgbwValues{}, "new OFF fixture has zero physical RGBW");
    expect_true(!first->actual_power(), "new OFF fixture factual power is OFF");
}

void test_fixture_rgb_color_temperature_semantics() {
    dmxwb::Fixture fixture{10, "Fixture"};
    fixture.set_power(true);

    expect_true(fixture.set_temperature(50), "temperature 50 accepted");
    expect_true(
        fixture.saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 128},
        "temperature 50 sets RGB to 255 and rounded W to 128");
    expect_true(fixture.temperature() == 50, "temperature setting is stored");

    fixture.set_red(17);
    expect_true(
        fixture.saved_rgbw() == dmxwb::RgbwValues{17, 255, 255, 0},
        "individual red preserves G/B and immediately clears W");

    fixture.set_green(23);
    expect_true(
        fixture.saved_rgbw() == dmxwb::RgbwValues{17, 23, 255, 0},
        "individual green preserves R/B and keeps W cleared");

    fixture.set_blue(31);
    expect_true(
        fixture.saved_rgbw() == dmxwb::RgbwValues{17, 23, 31, 0},
        "individual blue preserves R/G and keeps W cleared");

    fixture.set_color(1, 2, 3);
    expect_true(
        fixture.saved_rgbw() == dmxwb::RgbwValues{1, 2, 3, 0},
        "Color replaces RGB together and clears W");
    expect_true(fixture.temperature() == 50, "RGB/Color takeover does not destroy last temperature setting");

    expect_true(fixture.set_temperature(0), "temperature 0 accepted");
    expect_true(
        fixture.saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 0},
        "temperature 0 maps to 255/255/255/0");

    expect_true(fixture.set_temperature(50), "temperature 50 accepted again");
    expect_true(
        fixture.saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 128},
        "temperature 50 maps to rounded white 128");

    expect_true(fixture.set_temperature(100), "temperature 100 accepted");
    expect_true(
        fixture.saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 255},
        "temperature 100 maps to full RGBW");

    const auto before_invalid = fixture.saved_rgbw();
    expect_true(!fixture.set_temperature(101), "temperature above 100 rejected");
    expect_true(fixture.saved_rgbw() == before_invalid, "invalid temperature does not mutate saved RGBW");
    expect_true(fixture.temperature() == 100, "invalid temperature does not mutate setting");
}

void test_fixture_brightness_power_and_reset() {
    dmxwb::Fixture fixture{11, "Fixture"};
    fixture.set_color(255, 128, 1);
    fixture.set_power(true);

    expect_true(fixture.set_brightness(50), "brightness 50 accepted");
    expect_true(
        fixture.actual_rgbw() == dmxwb::RgbwValues{127, 64, 0, 0},
        "brightness uses saved_channel * percent / 100 for all channels");
    expect_true(fixture.actual_power(), "non-zero scaled output is factually ON");

    const auto saved_before_off = fixture.saved_rgbw();
    fixture.set_power(false);
    expect_true(fixture.actual_rgbw() == dmxwb::RgbwValues{}, "Power OFF forces physical RGBW to zero");
    expect_true(fixture.saved_rgbw() == saved_before_off, "Power OFF preserves saved RGBW");
    expect_true(fixture.brightness() == 50, "Power OFF preserves brightness");

    fixture.set_power(true);
    expect_true(
        fixture.actual_rgbw() == dmxwb::RgbwValues{127, 64, 0, 0},
        "Power ON restores saved state through current brightness");

    expect_true(fixture.set_brightness(0), "brightness 0 accepted");
    expect_true(fixture.requested_power(), "brightness 0 does not change requested power");
    expect_true(!fixture.actual_power(), "requested ON with brightness 0 is factually OFF");

    expect_true(!fixture.set_brightness(101), "brightness above 100 rejected");
    expect_true(fixture.brightness() == 0, "invalid brightness does not mutate setting");

    fixture.reset();
    expect_true(fixture.requested_power(), "Reset requests Power ON");
    expect_true(fixture.brightness() == 100, "Reset restores brightness 100");
    expect_true(fixture.temperature() == 100, "Reset restores temperature 100");
    expect_true(
        fixture.saved_rgbw() == dmxwb::RgbwValues{255, 255, 255, 255},
        "Reset restores saved RGBW to full");
    expect_true(
        fixture.actual_rgbw() == dmxwb::RgbwValues{255, 255, 255, 255},
        "Reset produces full physical RGBW");
    expect_true(fixture.actual_power(), "Reset fixture is factually ON");
}

void test_fixture_addressing_and_stable_ids() {
    dmxwb::FixtureCollection fixtures;

    expect_true(fixtures.reconfigure(75, 1), "75 RGBW fixtures fit exactly in physical slots 1..300");
    expect_true(fixtures.physical_slot_count() == std::optional<std::size_t>{300}, "75 fixtures produce slot_count 300");
    expect_true(fixtures.fixture_start_address(0) == std::optional<std::size_t>{1}, "first fixture starts at channel 1");
    expect_true(fixtures.fixture_start_address(74) == std::optional<std::size_t>{297}, "75th fixture starts at channel 297");
    expect_true(!fixtures.fixture_start_address(75).has_value(), "fixture address outside collection rejected");

    expect_true(!fixtures.reconfigure(75, 2), "configuration crossing physical slot 300 rejected");
    expect_true(fixtures.fixture_count() == 75 && fixtures.start_address() == 1,
                "invalid reconfigure leaves existing configuration unchanged");

    expect_true(fixtures.reconfigure(1, 297), "single RGBW fixture ending exactly at 300 accepted");
    expect_true(!fixtures.set_start_address(298), "single RGBW fixture crossing slot 300 rejected");
    expect_true(fixtures.start_address() == 297, "invalid start-address change leaves mapping unchanged");

    dmxwb::FixtureCollection identity;
    expect_true(identity.reconfigure(3, 1), "identity collection creates three fixtures");
    auto* first = identity.fixture_at(0);
    auto* third = identity.fixture_at(2);
    expect_true(first != nullptr && third != nullptr, "identity fixtures available");
    if (!first || !third) {
        return;
    }

    const auto first_id = first->id();
    const auto removed_id = third->id();
    first->set_name("Front");
    first->set_power(true);
    first->set_color(10, 20, 30);

    expect_true(identity.set_start_address(9), "valid Start Address change accepted");
    first = identity.fixture_at(0);
    expect_true(first != nullptr && first->id() == first_id, "Start Address change preserves fixture ID");
    if (!first) {
        return;
    }
    expect_true(first->name() == "Front", "Start Address change preserves Name");
    expect_true(first->saved_rgbw() == dmxwb::RgbwValues{10, 20, 30, 0},
                "Start Address change preserves saved RGBW");
    expect_true(first->requested_power(), "Start Address change preserves Power");

    expect_true(identity.set_fixture_count(2), "shrinking collection removes last fixture");
    expect_true(identity.set_fixture_count(3), "growing collection creates a new last fixture");
    const auto* replacement = identity.fixture_at(2);
    expect_true(replacement != nullptr, "replacement fixture exists");
    if (replacement) {
        expect_true(replacement->id() > removed_id, "removed Fixture ID is never reused");
        expect_true(replacement->name() == "Светильник 3", "new fixture gets default name for current position");
    }

    dmxwb::FixtureCollection empty;
    expect_true(empty.reconfigure(0, 1), "Fixture Count 0 is valid");
    expect_true(empty.physical_slot_count() == std::optional<std::size_t>{0}, "zero fixtures produce zero physical slots");
    const auto empty_snapshot = empty.build_snapshot(9);
    expect_true(empty_snapshot != nullptr && empty_snapshot->slot_count() == 0,
                "zero-fixture collection builds a valid zero-slot snapshot");
}

void test_fixture_whole_snapshot_rebuild() {
    dmxwb::FixtureCollection fixtures;
    expect_true(fixtures.reconfigure(2, 5), "snapshot collection maps two fixtures from start channel 5");

    auto* first = fixtures.fixture_at(0);
    auto* second = fixtures.fixture_at(1);
    expect_true(first != nullptr && second != nullptr, "snapshot fixtures available");
    if (!first || !second) {
        return;
    }

    first->set_power(true);
    first->set_color(10, 20, 30);

    second->set_power(true);
    expect_true(second->set_temperature(50), "second fixture temperature set");
    expect_true(second->set_brightness(50), "second fixture brightness set");

    const auto snapshot_one = fixtures.build_snapshot(100);
    expect_true(snapshot_one != nullptr, "whole Fixture snapshot built");
    if (!snapshot_one) {
        return;
    }

    expect_true(snapshot_one->slot_count() == 12, "start 5 plus two RGBW fixtures produces slot_count 12");
    expect_true(snapshot_one->generation() == 100, "Fixture snapshot preserves supplied generation");
    expect_true(snapshot_one->channel(1) == std::optional<std::uint8_t>{0}, "unused channel before Start Address is zero");
    expect_true(snapshot_one->channel(4) == std::optional<std::uint8_t>{0}, "all leading unused channels remain zero");
    expect_true(snapshot_one->channel(5) == std::optional<std::uint8_t>{10}, "fixture 1 R mapped to channel 5");
    expect_true(snapshot_one->channel(6) == std::optional<std::uint8_t>{20}, "fixture 1 G mapped to channel 6");
    expect_true(snapshot_one->channel(7) == std::optional<std::uint8_t>{30}, "fixture 1 B mapped to channel 7");
    expect_true(snapshot_one->channel(8) == std::optional<std::uint8_t>{0}, "fixture 1 W mapped to channel 8");
    expect_true(snapshot_one->channel(9) == std::optional<std::uint8_t>{127}, "fixture 2 scaled R mapped to channel 9");
    expect_true(snapshot_one->channel(10) == std::optional<std::uint8_t>{127}, "fixture 2 scaled G mapped to channel 10");
    expect_true(snapshot_one->channel(11) == std::optional<std::uint8_t>{127}, "fixture 2 scaled B mapped to channel 11");
    expect_true(snapshot_one->channel(12) == std::optional<std::uint8_t>{64}, "fixture 2 scaled W mapped to channel 12");

    first->set_color(99, 88, 77);
    const auto snapshot_two = fixtures.build_snapshot(101);
    expect_true(snapshot_two != nullptr, "second whole Fixture snapshot built");
    if (!snapshot_two) {
        return;
    }

    expect_true(snapshot_one->channel(5) == std::optional<std::uint8_t>{10},
                "previous immutable snapshot is unchanged after Fixture mutation");
    expect_true(snapshot_one->generation() == 100, "previous snapshot generation remains unchanged");
    expect_true(snapshot_two->channel(5) == std::optional<std::uint8_t>{99},
                "new snapshot contains latest whole Fixture state");
    expect_true(snapshot_two->generation() == 101, "new snapshot receives new generation");
}

}  // namespace

int main() {
    expect_equal(dmxwb::application_name(), "dmxwb", "application name");
    expect_equal(dmxwb::application_version(), "0.1.1", "application version");

    test_channel_boundaries();
    test_slot_count_helpers();
    test_start_code_and_payload_indexing();
    test_immutable_publication();
    test_dmx_diagnostic_patterns();
    test_clock_abstraction();
    test_fixed_physical_output_profile();
    test_preallocated_mailbox_frame_boundary();
    test_mailbox_concurrent_whole_frames();
    test_absolute_frame_schedule_no_drift();
    test_snapshot_switch_only_between_frames();
    test_serial_failure_reopen_recovery();
    test_missed_deadline_is_counted_and_grid_recovers();
    test_production_dmx_output_keeps_factual_state_without_counters();

    test_fixture_initial_state_and_identity();
    test_fixture_rgb_color_temperature_semantics();
    test_fixture_brightness_power_and_reset();
    test_fixture_addressing_and_stable_ids();
    test_fixture_whole_snapshot_rebuild();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All tests passed\n";
    return 0;
}
