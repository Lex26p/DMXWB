#pragma once

#include "dmxwb/artnet_core.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>

namespace dmxwb {

inline constexpr std::size_t kArtNetRuntimeReceiveBufferSize = 2048;
inline constexpr auto kArtNetPollReplyMaximumDelay = std::chrono::seconds{1};
inline constexpr auto kArtNetDefaultRebindDelay = std::chrono::seconds{1};
inline constexpr std::size_t kArtNetDefaultMaxDatagramsPerStep = 64;
inline constexpr std::size_t kArtNetDefaultPendingPollReplyLimit = 64;

enum class ArtNetTransportReceiveStatus {
    datagram,
    no_data,
    error,
};

struct ArtNetTransportReceiveResult final {
    ArtNetTransportReceiveStatus status{ArtNetTransportReceiveStatus::no_data};
    ArtNetIpv4Address source_ip{};
    ArtNetIpv4Address local_ip{};
    std::size_t captured_size{0};
    std::size_t datagram_size{0};
};

class IArtNetDatagramTransport {
public:
    virtual ~IArtNetDatagramTransport() = default;

    [[nodiscard]] virtual bool open_and_bind(std::uint16_t port) noexcept = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    [[nodiscard]] virtual ArtNetTransportReceiveResult receive(
        std::span<std::uint8_t> buffer) noexcept = 0;
    [[nodiscard]] virtual bool send_to(
        ArtNetIpv4Address destination,
        std::uint16_t port,
        std::span<const std::uint8_t> payload) noexcept = 0;
    virtual void close() noexcept = 0;
};

class IArtNetPollReplyDelaySource {
public:
    virtual ~IArtNetPollReplyDelaySource() = default;
    [[nodiscard]] virtual std::chrono::milliseconds next_delay() noexcept = 0;
};

struct ArtNetRuntimeConfig final {
    ArtNetCoreConfig core{};
    ArtNetPollReplyIdentity poll_reply_identity{};
    std::chrono::milliseconds rebind_delay{
        std::chrono::duration_cast<std::chrono::milliseconds>(kArtNetDefaultRebindDelay)};
    std::size_t max_datagrams_per_step{kArtNetDefaultMaxDatagramsPerStep};
    std::size_t pending_poll_reply_limit{kArtNetDefaultPendingPollReplyLimit};
};

struct ArtNetRuntimeDiagnostics final {
    bool transport_open{false};
    std::uint64_t bind_attempts{0};
    std::uint64_t bind_failures{0};
    std::uint64_t transport_recoveries{0};
    std::uint64_t datagrams_received{0};
    std::uint64_t receive_errors{0};
    std::uint64_t send_errors{0};
    std::uint64_t core_rejections{0};
    std::uint64_t conflicts{0};
    std::uint64_t source_lost_events{0};
    std::uint64_t snapshots_published{0};
    std::uint64_t poll_replies_scheduled{0};
    std::uint64_t poll_replies_sent{0};
    std::uint64_t poll_replies_dropped{0};
    std::uint64_t poll_replies_not_built{0};
    std::uint64_t delay_values_clamped{0};
};

class ArtNetRuntime final {
public:
    using time_point = ArtNetCore::time_point;

    [[nodiscard]] static std::unique_ptr<ArtNetRuntime> create(
        ArtNetRuntimeConfig config,
        IArtNetDatagramTransport& transport,
        IArtNetPollReplyDelaySource& delay_source) noexcept;

    ~ArtNetRuntime();

    ArtNetRuntime(const ArtNetRuntime&) = delete;
    ArtNetRuntime& operator=(const ArtNetRuntime&) = delete;
    ArtNetRuntime(ArtNetRuntime&&) = delete;
    ArtNetRuntime& operator=(ArtNetRuntime&&) = delete;

    void step(time_point now) noexcept;
    void shutdown() noexcept;
    void set_artnet_output_active(bool active) noexcept;

    [[nodiscard]] std::shared_ptr<const DmxSnapshot> latest_physical_snapshot() const noexcept;
    [[nodiscard]] const ArtNetCore& core() const noexcept;
    [[nodiscard]] const ArtNetRuntimeDiagnostics& diagnostics() const noexcept;
    [[nodiscard]] std::size_t pending_poll_replies() const noexcept;

private:
    struct PendingPollReply final {
        ArtNetIpv4Address destination{};
        ArtNetIpv4Address local_ip{};
        time_point due{};
    };

    ArtNetRuntime(
        ArtNetRuntimeConfig config,
        ArtNetCore core,
        IArtNetDatagramTransport& transport,
        IArtNetPollReplyDelaySource& delay_source) noexcept;

    void try_bind(time_point now) noexcept;
    void process_received_datagrams(time_point now) noexcept;
    void process_core_result(
        const ArtNetProcessResult& result,
        ArtNetIpv4Address source_ip,
        ArtNetIpv4Address local_ip,
        time_point now) noexcept;
    void process_core_tick(time_point now) noexcept;
    void publish_snapshot_if_changed() noexcept;
    void schedule_poll_reply(
        ArtNetIpv4Address destination,
        ArtNetIpv4Address local_ip,
        time_point now) noexcept;
    void send_due_poll_replies(time_point now) noexcept;
    void handle_transport_failure(time_point now, bool receive_failure) noexcept;
    void clear_pending_poll_replies() noexcept;

    [[nodiscard]] std::chrono::milliseconds next_poll_reply_delay() noexcept;

    ArtNetRuntimeConfig config_{};
    ArtNetCore core_;
    IArtNetDatagramTransport& transport_;
    IArtNetPollReplyDelaySource& delay_source_;
    ArtNetRuntimeDiagnostics diagnostics_{};
    std::array<std::uint8_t, kArtNetRuntimeReceiveBufferSize> receive_buffer_{};
    std::deque<PendingPollReply> pending_poll_replies_;
    std::optional<time_point> next_bind_attempt_;
    bool transport_failure_seen_{false};
    std::uint64_t last_published_revision_{0};

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const DmxSnapshot>> latest_snapshot_;
#else
    std::shared_ptr<const DmxSnapshot> latest_snapshot_;
#endif
};

}  // namespace dmxwb
