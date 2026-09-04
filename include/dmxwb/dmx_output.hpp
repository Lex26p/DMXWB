#pragma once

#include "dmxwb/dmx_snapshot.hpp"
#include "dmxwb/dmx_transport.hpp"
#include "dmxwb/instrumentation.hpp"
#include "dmxwb/monotonic_clock.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace dmxwb {

// Fixed production profile proven on the WB8 acceptance target. The Art-Net
// side may retain 512 channels internally, but physical RS-485 output accepts
// at most kDmxPhysicalMaxSlots and always runs on this cadence.
inline constexpr std::uint32_t kDmxOutputRefreshHz = 44;

[[nodiscard]] std::chrono::nanoseconds minimum_dmx_frame_time(std::size_t slot_count) noexcept;

class DmxOutputMailbox final {
public:
    DmxOutputMailbox();

    // Physical output accepts snapshots up to the DMXWB 300-slot product limit.
    [[nodiscard]] bool publish(const DmxSnapshot& snapshot);

    // Single-reader API for the DmxOutput thread. The returned view stays valid
    // until the next acquire() call on that reader. Writers never modify the
    // current front slot.
    [[nodiscard]] DmxFrameView acquire() noexcept;

    [[nodiscard]] std::size_t published_slot_count() const noexcept;
    [[nodiscard]] DmxSnapshot::Generation published_generation() const noexcept;

private:
    struct Slot final {
        DmxSnapshot::ChannelData channels{};
        std::size_t slot_count{0};
        DmxSnapshot::Generation generation{0};
    };

    static constexpr std::size_t kSlotCount = 3;
    static constexpr std::uint32_t kIndexMask = 0x3U;
    static constexpr std::uint32_t kDirtyBit = 0x4U;

    std::array<Slot, kSlotCount> slots_{};
    std::atomic<std::uint32_t> middle_state_{1U};
    std::size_t front_index_{0};
    std::size_t back_index_{2};
    std::atomic<std::size_t> published_slot_count_{0};
    std::atomic<DmxSnapshot::Generation> published_generation_{0};
    std::mutex publish_mutex_;
};

struct DmxOutputDiagnostics final {
    std::uint64_t frames_sent{0};
    std::uint64_t open_attempts{0};
    std::uint64_t reopen_attempts{0};
    std::uint64_t open_failures{0};
    std::uint64_t send_failures{0};
    std::uint64_t recoveries{0};
    std::uint64_t missed_deadlines{0};
    DmxSnapshot::Generation active_generation{0};
    std::uint32_t active_refresh_hz{kDmxOutputRefreshHz};
    bool serial_open{false};
    std::chrono::nanoseconds max_send_duration{};
    std::chrono::nanoseconds max_transport_overhead{};
    std::string last_error;
};

struct DmxOutputOperationalState final {
    bool running{false};
    bool serial_open{false};
    std::size_t slot_count{0};
    DmxSnapshot::Generation active_generation{0};
    std::uint32_t refresh_hz{kDmxOutputRefreshHz};
    std::string last_error;
};

enum class DmxOutputStepKind {
    wait_until,
    frame_sent,
    transport_error,
};

struct DmxOutputStep final {
    DmxOutputStepKind kind{DmxOutputStepKind::wait_until};
    MonotonicClock::time_point wake_at{};
};

class DmxOutputLoop final {
public:
    DmxOutputLoop(
        DmxTransportInterface& transport,
        DmxOutputMailbox& mailbox,
        MonotonicClock& clock,
        std::chrono::milliseconds reopen_interval = std::chrono::milliseconds{250},
        InstrumentationMode instrumentation_mode = InstrumentationMode::engineering);

    [[nodiscard]] DmxOutputStep step();
    void shutdown() noexcept;

    [[nodiscard]] DmxOutputDiagnostics diagnostics() const;
    [[nodiscard]] DmxOutputOperationalState operational_state() const;
    [[nodiscard]] InstrumentationMode instrumentation_mode() const noexcept;

private:
    void set_error(std::string message);
    void update_max_send_duration(std::chrono::nanoseconds value) noexcept;
    void update_max_transport_overhead(std::chrono::nanoseconds value) noexcept;
    [[nodiscard]] static MonotonicClock::duration active_period() noexcept;

    DmxTransportInterface& transport_;
    DmxOutputMailbox& mailbox_;
    MonotonicClock& clock_;
    std::chrono::milliseconds reopen_interval_;
    InstrumentationMode instrumentation_mode_{InstrumentationMode::engineering};

    std::optional<MonotonicClock::time_point> next_open_attempt_;
    std::optional<MonotonicClock::time_point> next_frame_start_;
    bool has_attempted_open_{false};
    bool transport_error_seen_{false};

    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> open_attempts_{0};
    std::atomic<std::uint64_t> reopen_attempts_{0};
    std::atomic<std::uint64_t> open_failures_{0};
    std::atomic<std::uint64_t> send_failures_{0};
    std::atomic<std::uint64_t> recoveries_{0};
    std::atomic<std::uint64_t> missed_deadlines_{0};
    std::atomic<DmxSnapshot::Generation> active_generation_{0};
    std::atomic<bool> serial_open_{false};
    std::atomic<std::int64_t> max_send_duration_ns_{0};
    std::atomic<std::int64_t> max_transport_overhead_ns_{0};

    mutable std::mutex error_mutex_;
    std::string last_error_;
};

struct DmxOutputConfig final {
    std::string port{std::string{kDefaultDmxPort}};
    std::chrono::milliseconds reopen_interval{250};
};

class DmxOutput final {
public:
    explicit DmxOutput(
        DmxOutputConfig config = {},
        InstrumentationMode instrumentation_mode = InstrumentationMode::engineering);
    DmxOutput(
        DmxOutputConfig config,
        std::unique_ptr<DmxTransportInterface> transport,
        std::unique_ptr<MonotonicClock> clock,
        InstrumentationMode instrumentation_mode = InstrumentationMode::engineering);
    ~DmxOutput();

    DmxOutput(const DmxOutput&) = delete;
    DmxOutput& operator=(const DmxOutput&) = delete;
    DmxOutput(DmxOutput&&) = delete;
    DmxOutput& operator=(DmxOutput&&) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] bool publish_snapshot(const DmxSnapshot& snapshot);
    [[nodiscard]] DmxOutputDiagnostics diagnostics() const;
    [[nodiscard]] DmxOutputOperationalState operational_state() const;
    [[nodiscard]] InstrumentationMode instrumentation_mode() const noexcept;

private:
    void worker_main() noexcept;

    DmxOutputConfig config_;
    DmxOutputMailbox mailbox_;
    std::unique_ptr<DmxTransportInterface> transport_;
    std::unique_ptr<MonotonicClock> clock_;
    std::unique_ptr<DmxOutputLoop> loop_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

}  // namespace dmxwb
