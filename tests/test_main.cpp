#include "dmxwb/app_info.hpp"
#include "dmxwb/dmx_output.hpp"
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
    FakeMonotonicClock clock{expected};
    expect_true(clock.now() == expected, "monotonic clock interface supports deterministic fake time");

    const auto deadline = expected + std::chrono::milliseconds{50};
    clock.sleep_until(deadline);
    expect_true(clock.now() == deadline, "fake monotonic clock advances to absolute deadline");
}

void test_refresh_validation() {
    const auto short_frame_44 = dmxwb::check_dmx_refresh_rate(4, 44);
    expect_true(short_frame_44.valid, "44 Hz accepted for short four-slot frame");

    const auto full_frame_44 = dmxwb::check_dmx_refresh_rate(512, 44);
    expect_true(full_frame_44.valid, "44 Hz is theoretically possible for full 512-slot hardware-BREAK frame");
    expect_true(full_frame_44.max_supported_hz == 44, "full 512-slot theoretical maximum reaches interface cap 44 Hz");

    const auto full_frame_with_measured_overhead =
        dmxwb::check_dmx_refresh_rate(512, 44, std::chrono::milliseconds{1});
    expect_true(
        !full_frame_with_measured_overhead.valid,
        "measured 512-slot transport overhead rejects 44 Hz when real frame exceeds period");

    expect_true(!dmxwb::check_dmx_refresh_rate(4, 9).valid, "refresh below 10 Hz rejected");
    expect_true(!dmxwb::check_dmx_refresh_rate(4, 45).valid, "refresh above 44 Hz rejected");

    const auto with_overhead = dmxwb::check_dmx_refresh_rate(4, 10, std::chrono::milliseconds{100});
    expect_true(!with_overhead.valid, "measured transport overhead participates in refresh feasibility");
}

void test_high_startup_refresh_waits_for_transport_measurement() {
    FakeMonotonicClock clock;
    FakeDmxTransport transport{clock};
    transport.set_send_duration(std::chrono::milliseconds{24});

    dmxwb::DmxOutputMailbox mailbox;
    const auto snapshot = make_filled_snapshot(512, 0x22, 21);
    expect_true(snapshot != nullptr, "high-startup snapshot built");
    if (!snapshot) {
        return;
    }
    expect_true(mailbox.publish(*snapshot), "high-startup snapshot published");

    std::atomic<std::uint32_t> refresh_hz{44};
    dmxwb::DmxOutputLoop loop{transport, mailbox, clock, refresh_hz};
    expect_true(
        loop.diagnostics().active_refresh_hz == dmxwb::kDmxDefaultRefreshHz,
        "high startup request begins at safe default refresh before measurement");

    expect_true(drive_until_frames(loop, clock, 1), "high-startup first measurement frame sent");
    expect_true(
        loop.diagnostics().active_refresh_hz == dmxwb::kDmxDefaultRefreshHz,
        "first measurement frame remains on safe default refresh");

    expect_true(drive_until_frames(loop, clock, 2), "high-startup second frame sent after feasibility decision");
    const auto diagnostics = loop.diagnostics();
    expect_true(diagnostics.refresh_rejections == 1, "measured impossible high startup refresh rejected once");
    expect_true(
        diagnostics.active_refresh_hz == dmxwb::kDmxDefaultRefreshHz,
        "rejected high startup refresh stays at safe default");
    expect_true(
        refresh_hz.load(std::memory_order_acquire) == dmxwb::kDmxDefaultRefreshHz,
        "rejected requested refresh is normalized to active refresh");
    loop.shutdown();
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

    std::atomic<std::uint32_t> refresh_hz{10};
    dmxwb::DmxOutputLoop loop{transport, mailbox, clock, refresh_hz};
    expect_true(drive_until_frames(loop, clock, 3), "absolute scheduler produced three frames");

    const auto& starts = transport.send_start_times();
    expect_true(starts.size() >= 3, "absolute scheduler recorded three frame starts");
    if (starts.size() >= 3) {
        expect_true(starts[0] == dmxwb::MonotonicClock::time_point{}, "first frame starts at T0");
        expect_true(starts[1] == dmxwb::MonotonicClock::time_point{std::chrono::milliseconds{100}}, "second frame starts at T0 + period");
        expect_true(starts[2] == dmxwb::MonotonicClock::time_point{std::chrono::milliseconds{200}}, "third frame starts at T0 + 2*period without drift");
    }
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

    std::atomic<std::uint32_t> refresh_hz{30};
    dmxwb::DmxOutputLoop loop{transport, mailbox, clock, refresh_hz};
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

    std::atomic<std::uint32_t> refresh_hz{30};
    dmxwb::DmxOutputLoop loop{
        transport,
        mailbox,
        clock,
        refresh_hz,
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

void test_runtime_refresh_change_keeps_absolute_schedule() {
    FakeMonotonicClock clock;
    FakeDmxTransport transport{clock};
    transport.set_send_duration(std::chrono::milliseconds{1});

    dmxwb::DmxOutputMailbox mailbox;
    const auto snapshot = make_filled_snapshot(4, 0x55, 12);
    expect_true(snapshot != nullptr, "refresh-change snapshot built");
    if (!snapshot) {
        return;
    }
    expect_true(mailbox.publish(*snapshot), "refresh-change snapshot published");

    std::atomic<std::uint32_t> refresh_hz{30};
    dmxwb::DmxOutputLoop loop{transport, mailbox, clock, refresh_hz};
    expect_true(drive_until_frames(loop, clock, 1), "refresh-change first frame sent");

    refresh_hz.store(20, std::memory_order_release);
    expect_true(drive_until_frames(loop, clock, 3), "refresh-change produced frames at new rate");

    const auto& starts = transport.send_start_times();
    expect_true(starts.size() >= 3, "refresh-change transport recorded three frames");
    if (starts.size() >= 3) {
        const auto first_30hz_deadline = dmxwb::MonotonicClock::time_point{std::chrono::nanoseconds{33'333'333}};
        const auto next_20hz_deadline = dmxwb::MonotonicClock::time_point{std::chrono::nanoseconds{83'333'333}};
        expect_true(starts[1] == first_30hz_deadline, "refresh change applies at next frame boundary without closing serial");
        expect_true(starts[2] == next_20hz_deadline, "new 20 Hz period advances from absolute boundary");
    }
    expect_true(loop.diagnostics().active_refresh_hz == 20, "active refresh diagnostics report runtime change");
    expect_true(transport.open_calls() == 1, "runtime refresh change does not reopen serial");
    loop.shutdown();
}

void test_missed_deadline_is_counted_and_grid_recovers() {
    FakeMonotonicClock clock;
    FakeDmxTransport transport{clock};
    transport.set_send_duration(std::chrono::milliseconds{120});

    dmxwb::DmxOutputMailbox mailbox;
    const auto snapshot = make_filled_snapshot(4, 0x66, 13);
    expect_true(snapshot != nullptr, "deadline snapshot built");
    if (!snapshot) {
        return;
    }
    expect_true(mailbox.publish(*snapshot), "deadline snapshot published");

    std::atomic<std::uint32_t> refresh_hz{10};
    dmxwb::DmxOutputLoop loop{transport, mailbox, clock, refresh_hz};
    expect_true(drive_until_frames(loop, clock, 2), "deadline test produced two frames");
    expect_true(loop.diagnostics().missed_deadlines >= 1, "frame longer than period increments missed deadline counter");

    const auto& starts = transport.send_start_times();
    expect_true(starts.size() >= 2, "deadline test recorded two starts");
    if (starts.size() >= 2) {
        expect_true(starts[1] == dmxwb::MonotonicClock::time_point{std::chrono::milliseconds{200}}, "scheduler skips missed 100 ms boundary and returns to absolute grid");
    }
    loop.shutdown();
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
    test_refresh_validation();
    test_high_startup_refresh_waits_for_transport_measurement();
    test_preallocated_mailbox_frame_boundary();
    test_mailbox_concurrent_whole_frames();
    test_absolute_frame_schedule_no_drift();
    test_snapshot_switch_only_between_frames();
    test_serial_failure_reopen_recovery();
    test_runtime_refresh_change_keeps_absolute_schedule();
    test_missed_deadline_is_counted_and_grid_recovers();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All tests passed\n";
    return 0;
}
