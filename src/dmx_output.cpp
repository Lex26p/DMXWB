#include "dmxwb/dmx_output.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dmxwb {
namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000LL;
constexpr std::int64_t kBreakBaud = 38'400;
constexpr std::int64_t kDataBaud = 250'000;
constexpr std::int64_t kSerialBitsPerByte8N2 = 11;

[[nodiscard]] constexpr std::int64_t ceil_div(std::int64_t numerator, std::int64_t denominator) noexcept {
    return (numerator + denominator - 1) / denominator;
}

[[nodiscard]] constexpr std::chrono::nanoseconds break_wire_time() noexcept {
    return std::chrono::nanoseconds{
        ceil_div(kSerialBitsPerByte8N2 * kNanosecondsPerSecond, kBreakBaud)};
}

[[nodiscard]] constexpr std::chrono::nanoseconds data_byte_wire_time() noexcept {
    return std::chrono::nanoseconds{
        ceil_div(kSerialBitsPerByte8N2 * kNanosecondsPerSecond, kDataBaud)};
}

void update_atomic_max(std::atomic<std::int64_t>& target, std::int64_t value) noexcept {
    auto observed = target.load(std::memory_order_relaxed);
    while (observed < value &&
           !target.compare_exchange_weak(
               observed,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

}  // namespace

std::chrono::nanoseconds minimum_dmx_frame_time(
    std::size_t slot_count,
    std::chrono::nanoseconds measured_transport_overhead) noexcept {
    if (slot_count > kDmxMaxChannels) {
        slot_count = kDmxMaxChannels;
    }
    if (measured_transport_overhead.count() < 0) {
        measured_transport_overhead = std::chrono::nanoseconds{0};
    }

    const auto data_bytes = slot_count + 1;  // Start Code + active slots.
    const auto data_time = data_byte_wire_time() * static_cast<std::int64_t>(data_bytes);
    return break_wire_time() + data_time + measured_transport_overhead;
}

DmxRefreshCheck check_dmx_refresh_rate(
    std::size_t slot_count,
    std::uint32_t requested_hz,
    std::chrono::nanoseconds measured_transport_overhead) noexcept {
    const auto minimum_frame = minimum_dmx_frame_time(slot_count, measured_transport_overhead);
    const auto minimum_ns = std::max<std::int64_t>(minimum_frame.count(), 1);
    const auto physical_hz = static_cast<std::uint64_t>(kNanosecondsPerSecond / minimum_ns);
    const auto capped_hz = std::min<std::uint64_t>(physical_hz, kDmxMaxRefreshHz);
    const auto max_supported = static_cast<std::uint32_t>(capped_hz);

    const bool in_interface_range =
        requested_hz >= kDmxMinRefreshHz && requested_hz <= kDmxMaxRefreshHz;
    const bool physically_possible = requested_hz <= max_supported;

    return DmxRefreshCheck{
        in_interface_range && physically_possible,
        requested_hz,
        max_supported,
        minimum_frame};
}

DmxOutputMailbox::DmxOutputMailbox() = default;

bool DmxOutputMailbox::publish(const DmxSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock{publish_mutex_};

    auto& target = slots_[back_index_];
    target.channels.fill(0);
    const auto active = snapshot.active_channels();
    std::copy(active.begin(), active.end(), target.channels.begin());
    target.slot_count = snapshot.slot_count();
    target.generation = snapshot.generation();

    published_slot_count_.store(target.slot_count, std::memory_order_release);
    published_generation_.store(target.generation, std::memory_order_release);

    const auto new_middle = static_cast<std::uint32_t>(back_index_) | kDirtyBit;
    const auto previous_middle = middle_state_.exchange(new_middle, std::memory_order_acq_rel);
    back_index_ = static_cast<std::size_t>(previous_middle & kIndexMask);
    return true;
}

DmxFrameView DmxOutputMailbox::acquire() noexcept {
    const auto observed = middle_state_.load(std::memory_order_acquire);
    if ((observed & kDirtyBit) != 0U) {
        const auto previous_middle = middle_state_.exchange(
            static_cast<std::uint32_t>(front_index_),
            std::memory_order_acq_rel);
        if ((previous_middle & kDirtyBit) != 0U) {
            front_index_ = static_cast<std::size_t>(previous_middle & kIndexMask);
        }
    }

    const auto& front = slots_[front_index_];
    return DmxFrameView{
        kDmxStartCode,
        std::span<const std::uint8_t>{front.channels.data(), front.slot_count},
        front.generation};
}

std::size_t DmxOutputMailbox::published_slot_count() const noexcept {
    return published_slot_count_.load(std::memory_order_acquire);
}

DmxSnapshot::Generation DmxOutputMailbox::published_generation() const noexcept {
    return published_generation_.load(std::memory_order_acquire);
}

DmxOutputLoop::DmxOutputLoop(
    DmxTransportInterface& transport,
    DmxOutputMailbox& mailbox,
    MonotonicClock& clock,
    std::atomic<std::uint32_t>& requested_refresh_hz,
    std::chrono::milliseconds reopen_interval)
    : transport_(transport),
      mailbox_(mailbox),
      clock_(clock),
      requested_refresh_hz_(requested_refresh_hz),
      reopen_interval_(reopen_interval) {
    const auto requested = requested_refresh_hz_.load(std::memory_order_acquire);
    if (requested >= kDmxMinRefreshHz && requested <= kDmxMaxRefreshHz) {
        active_refresh_hz_.store(requested, std::memory_order_release);
    }
}

DmxOutputStep DmxOutputLoop::step() {
    auto now = clock_.now();

    if (!transport_.is_open()) {
        if (next_open_attempt_.has_value() && now < *next_open_attempt_) {
            return DmxOutputStep{DmxOutputStepKind::wait_until, *next_open_attempt_};
        }

        if (has_attempted_open_) {
            reopen_attempts_.fetch_add(1, std::memory_order_relaxed);
        } else {
            has_attempted_open_ = true;
        }
        open_attempts_.fetch_add(1, std::memory_order_relaxed);

        if (!transport_.open()) {
            open_failures_.fetch_add(1, std::memory_order_relaxed);
            serial_open_.store(false, std::memory_order_release);
            transport_error_seen_ = true;
            set_error(std::string{transport_.last_error()});
            next_open_attempt_ = now + reopen_interval_;
            next_frame_start_.reset();
            return DmxOutputStep{DmxOutputStepKind::transport_error, now};
        }

        serial_open_.store(true, std::memory_order_release);
        if (transport_error_seen_) {
            recoveries_.fetch_add(1, std::memory_order_relaxed);
            transport_error_seen_ = false;
        }
        set_error({});
        next_open_attempt_.reset();
        now = clock_.now();
        next_frame_start_ = now;
    }

    now = clock_.now();
    if (next_frame_start_.has_value() && now < *next_frame_start_) {
        return DmxOutputStep{DmxOutputStepKind::wait_until, *next_frame_start_};
    }

    const auto frame = mailbox_.acquire();
    const auto slot_count = frame.channels.size();
    const auto requested_refresh = requested_refresh_hz_.load(std::memory_order_acquire);
    const auto refresh_check = check_dmx_refresh_rate(
        slot_count,
        requested_refresh,
        max_observed_transport_overhead());

    if (!refresh_check.valid) {
        refresh_rejections_.fetch_add(1, std::memory_order_relaxed);
        requested_refresh_hz_.store(active_refresh_hz_.load(std::memory_order_acquire), std::memory_order_release);
    } else {
        active_refresh_hz_.store(requested_refresh, std::memory_order_release);
    }

    const auto scheduled_start = next_frame_start_.value_or(now);
    const auto send_started = clock_.now();

    if (!transport_.send_frame(frame)) {
        const auto failed_at = clock_.now();
        send_failures_.fetch_add(1, std::memory_order_relaxed);
        transport_error_seen_ = true;
        set_error(std::string{transport_.last_error()});
        transport_.close();
        serial_open_.store(false, std::memory_order_release);
        next_frame_start_.reset();
        next_open_attempt_ = failed_at + reopen_interval_;
        return DmxOutputStep{DmxOutputStepKind::transport_error, failed_at};
    }

    const auto send_finished = clock_.now();
    const auto send_duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(send_finished - send_started);
    update_max_send_duration(send_duration);

    const auto wire_time = minimum_dmx_frame_time(slot_count);
    if (send_duration > wire_time) {
        update_max_transport_overhead(send_duration - wire_time);
    }

    frames_sent_.fetch_add(1, std::memory_order_relaxed);
    active_generation_.store(frame.generation, std::memory_order_release);

    auto next_start = scheduled_start + active_period();
    std::uint64_t missed = 0;
    while (next_start <= send_finished) {
        next_start += active_period();
        ++missed;
    }
    if (missed != 0) {
        missed_deadlines_.fetch_add(missed, std::memory_order_relaxed);
    }
    next_frame_start_ = next_start;

    return DmxOutputStep{DmxOutputStepKind::frame_sent, next_start};
}

void DmxOutputLoop::shutdown() noexcept {
    transport_.close();
    serial_open_.store(false, std::memory_order_release);
    next_open_attempt_.reset();
    next_frame_start_.reset();
}

DmxOutputDiagnostics DmxOutputLoop::diagnostics() const {
    DmxOutputDiagnostics result{};
    result.frames_sent = frames_sent_.load(std::memory_order_acquire);
    result.open_attempts = open_attempts_.load(std::memory_order_acquire);
    result.reopen_attempts = reopen_attempts_.load(std::memory_order_acquire);
    result.open_failures = open_failures_.load(std::memory_order_acquire);
    result.send_failures = send_failures_.load(std::memory_order_acquire);
    result.recoveries = recoveries_.load(std::memory_order_acquire);
    result.missed_deadlines = missed_deadlines_.load(std::memory_order_acquire);
    result.refresh_rejections = refresh_rejections_.load(std::memory_order_acquire);
    result.active_generation = active_generation_.load(std::memory_order_acquire);
    result.active_refresh_hz = active_refresh_hz_.load(std::memory_order_acquire);
    result.serial_open = serial_open_.load(std::memory_order_acquire);
    result.max_send_duration = std::chrono::nanoseconds{max_send_duration_ns_.load(std::memory_order_acquire)};
    result.max_transport_overhead =
        std::chrono::nanoseconds{max_transport_overhead_ns_.load(std::memory_order_acquire)};
    {
        std::lock_guard<std::mutex> lock{error_mutex_};
        result.last_error = last_error_;
    }
    return result;
}

std::chrono::nanoseconds DmxOutputLoop::max_observed_transport_overhead() const noexcept {
    return std::chrono::nanoseconds{max_transport_overhead_ns_.load(std::memory_order_acquire)};
}

void DmxOutputLoop::set_error(std::string message) {
    std::lock_guard<std::mutex> lock{error_mutex_};
    last_error_ = std::move(message);
}

void DmxOutputLoop::update_max_send_duration(std::chrono::nanoseconds value) noexcept {
    update_atomic_max(max_send_duration_ns_, value.count());
}

void DmxOutputLoop::update_max_transport_overhead(std::chrono::nanoseconds value) noexcept {
    update_atomic_max(max_transport_overhead_ns_, value.count());
}

MonotonicClock::duration DmxOutputLoop::active_period() const noexcept {
    const auto refresh = std::max<std::uint32_t>(active_refresh_hz_.load(std::memory_order_acquire), 1U);
    const auto period_ns = kNanosecondsPerSecond / static_cast<std::int64_t>(refresh);
    return std::chrono::duration_cast<MonotonicClock::duration>(std::chrono::nanoseconds{period_ns});
}

DmxOutput::DmxOutput(DmxOutputConfig config)
    : DmxOutput(
          config,
          std::make_unique<DmxTransport>(config.port),
          std::make_unique<SteadyMonotonicClock>()) {}

DmxOutput::DmxOutput(
    DmxOutputConfig config,
    std::unique_ptr<DmxTransportInterface> transport,
    std::unique_ptr<MonotonicClock> clock)
    : config_(std::move(config)),
      requested_refresh_hz_(config_.refresh_hz),
      transport_(std::move(transport)),
      clock_(std::move(clock)) {
    if (!transport_ || !clock_) {
        throw std::invalid_argument("DmxOutput requires transport and monotonic clock");
    }
    if (config_.refresh_hz < kDmxMinRefreshHz || config_.refresh_hz > kDmxMaxRefreshHz) {
        throw std::invalid_argument("DmxOutput refresh must be in range 10..44 Hz");
    }
    if (config_.reopen_interval.count() < 1) {
        throw std::invalid_argument("DmxOutput reopen interval must be positive");
    }

    loop_ = std::make_unique<DmxOutputLoop>(
        *transport_,
        mailbox_,
        *clock_,
        requested_refresh_hz_,
        config_.reopen_interval);
}

DmxOutput::~DmxOutput() {
    stop();
}

bool DmxOutput::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }

    stop_requested_.store(false, std::memory_order_release);
    try {
        worker_ = std::thread{[this] { worker_main(); }};
    } catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void DmxOutput::stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false, std::memory_order_release);
}

bool DmxOutput::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool DmxOutput::publish_snapshot(const DmxSnapshot& snapshot) {
    const auto refresh_check = check_dmx_refresh_rate(
        snapshot.slot_count(),
        requested_refresh_hz_.load(std::memory_order_acquire),
        loop_->max_observed_transport_overhead());
    if (!refresh_check.valid) {
        return false;
    }
    return mailbox_.publish(snapshot);
}

bool DmxOutput::set_refresh_rate(std::uint32_t refresh_hz) noexcept {
    const auto refresh_check = check_dmx_refresh_rate(
        mailbox_.published_slot_count(),
        refresh_hz,
        loop_->max_observed_transport_overhead());
    if (!refresh_check.valid) {
        return false;
    }
    requested_refresh_hz_.store(refresh_hz, std::memory_order_release);
    return true;
}

std::uint32_t DmxOutput::requested_refresh_rate() const noexcept {
    return requested_refresh_hz_.load(std::memory_order_acquire);
}

DmxOutputDiagnostics DmxOutput::diagnostics() const {
    return loop_->diagnostics();
}

void DmxOutput::worker_main() noexcept {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        try {
            const auto result = loop_->step();
            if (result.kind == DmxOutputStepKind::wait_until) {
                clock_->sleep_until(result.wake_at);
            }
        } catch (...) {
            // A production worker must not unwind through std::thread. Transport/runtime errors
            // are represented by diagnostics; unexpected exceptions terminate this worker only.
            break;
        }
    }
    loop_->shutdown();
    running_.store(false, std::memory_order_release);
}

}  // namespace dmxwb
