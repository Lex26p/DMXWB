#include "dmxwb/artnet_runtime.hpp"

#include <algorithm>
#include <utility>

namespace dmxwb {
namespace {

[[nodiscard]] bool runtime_config_valid(const ArtNetRuntimeConfig& config) noexcept {
    return config.core.port_address <= kArtNetPortAddressMax &&
           config.rebind_delay.count() > 0 &&
           config.max_datagrams_per_step > 0 &&
           config.pending_poll_reply_limit > 0;
}

}  // namespace

std::unique_ptr<ArtNetRuntime> ArtNetRuntime::create(
    ArtNetRuntimeConfig config,
    IArtNetDatagramTransport& transport,
    IArtNetPollReplyDelaySource& delay_source) noexcept {
    if (!runtime_config_valid(config)) {
        return nullptr;
    }

    auto core = ArtNetCore::create(config.core);
    if (!core.has_value()) {
        return nullptr;
    }

    return std::unique_ptr<ArtNetRuntime>{new ArtNetRuntime(
        std::move(config), std::move(*core), transport, delay_source)};
}

ArtNetRuntime::ArtNetRuntime(
    ArtNetRuntimeConfig config,
    ArtNetCore core,
    IArtNetDatagramTransport& transport,
    IArtNetPollReplyDelaySource& delay_source) noexcept
    : config_(std::move(config)),
      core_(std::move(core)),
      transport_(transport),
      delay_source_(delay_source),
      artnet_output_active_(config_.poll_reply_identity.artnet_output_active),
      latest_snapshot_(std::shared_ptr<const DmxSnapshot>{}) {}

ArtNetRuntime::~ArtNetRuntime() {
    shutdown();
}

void ArtNetRuntime::step(time_point now) noexcept {
    if (!transport_.is_open()) {
        try_bind(now);
    }

    if (transport_.is_open()) {
        process_received_datagrams(now);
    }

    process_core_tick(now);
    publish_snapshot_if_changed();

    if (transport_.is_open()) {
        send_due_poll_replies(now);
    }

    diagnostics_.transport_open = transport_.is_open();
}

void ArtNetRuntime::shutdown() noexcept {
    clear_pending_poll_replies();
    if (transport_.is_open()) {
        transport_.close();
    }
    diagnostics_.transport_open = false;
    next_bind_attempt_.reset();
}

void ArtNetRuntime::set_artnet_output_active(bool active) noexcept {
    artnet_output_active_.store(active, std::memory_order_release);
}

std::shared_ptr<const DmxSnapshot> ArtNetRuntime::latest_physical_snapshot() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return latest_snapshot_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&latest_snapshot_, std::memory_order_acquire);
#endif
}

const ArtNetCore& ArtNetRuntime::core() const noexcept {
    return core_;
}

const ArtNetRuntimeDiagnostics& ArtNetRuntime::diagnostics() const noexcept {
    return diagnostics_;
}

std::size_t ArtNetRuntime::pending_poll_replies() const noexcept {
    return pending_poll_replies_.size();
}

void ArtNetRuntime::try_bind(time_point now) noexcept {
    if (next_bind_attempt_.has_value() && now < *next_bind_attempt_) {
        return;
    }

    ++diagnostics_.bind_attempts;
    if (transport_.open_and_bind(kArtNetUdpPort)) {
        diagnostics_.transport_open = true;
        next_bind_attempt_.reset();
        if (transport_failure_seen_) {
            ++diagnostics_.transport_recoveries;
            transport_failure_seen_ = false;
        }
        return;
    }

    ++diagnostics_.bind_failures;
    diagnostics_.transport_open = false;
    transport_failure_seen_ = true;
    next_bind_attempt_ = now + config_.rebind_delay;
}

void ArtNetRuntime::process_received_datagrams(time_point now) noexcept {
    for (std::size_t index = 0; index < config_.max_datagrams_per_step; ++index) {
        const auto receive_result = transport_.receive(receive_buffer_);
        if (receive_result.status == ArtNetTransportReceiveStatus::no_data) {
            return;
        }
        if (receive_result.status == ArtNetTransportReceiveStatus::error) {
            handle_transport_failure(now, true);
            return;
        }

        if (receive_result.captured_size > receive_buffer_.size() ||
            receive_result.captured_size > receive_result.datagram_size) {
            handle_transport_failure(now, true);
            return;
        }

        ++diagnostics_.datagrams_received;
        const auto packet = std::span<const std::uint8_t>{
            receive_buffer_.data(), receive_result.captured_size};
        const auto result = core_.process_datagram(packet, receive_result.source_ip, now);
        process_core_result(result, receive_result.source_ip, receive_result.local_ip, now);
        publish_snapshot_if_changed();

        if (!transport_.is_open()) {
            return;
        }
    }
}

void ArtNetRuntime::process_core_result(
    const ArtNetProcessResult& result,
    ArtNetIpv4Address source_ip,
    ArtNetIpv4Address local_ip,
    time_point now) noexcept {
    if (result.rejected()) {
        ++diagnostics_.core_rejections;
    }

    if (result.action == ArtNetAction::conflict) {
        ++diagnostics_.conflicts;
    } else if (result.action == ArtNetAction::source_lost) {
        ++diagnostics_.source_lost_events;
    } else if (result.action == ArtNetAction::poll_reply_requested) {
        schedule_poll_reply(source_ip, local_ip, now);
    }
}

void ArtNetRuntime::process_core_tick(time_point now) noexcept {
    const auto result = core_.tick(now);
    process_core_result(result, ArtNetIpv4Address{}, ArtNetIpv4Address{}, now);
}

void ArtNetRuntime::publish_snapshot_if_changed() noexcept {
    const auto revision = core_.committed_revision();
    if (!core_.has_committed_dmx() || revision == last_published_revision_) {
        return;
    }

    auto snapshot = core_.build_physical_snapshot(revision);
    if (!snapshot) {
        return;
    }

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    latest_snapshot_.store(std::move(snapshot), std::memory_order_release);
#else
    std::atomic_store_explicit(&latest_snapshot_, std::move(snapshot), std::memory_order_release);
#endif
    last_published_revision_ = revision;
    ++diagnostics_.snapshots_published;
}

void ArtNetRuntime::schedule_poll_reply(
    ArtNetIpv4Address destination,
    ArtNetIpv4Address local_ip,
    time_point now) noexcept {
    if (pending_poll_replies_.size() >= config_.pending_poll_reply_limit) {
        ++diagnostics_.poll_replies_dropped;
        return;
    }

    pending_poll_replies_.push_back(PendingPollReply{
        destination,
        local_ip,
        now + next_poll_reply_delay()});
    ++diagnostics_.poll_replies_scheduled;
}

void ArtNetRuntime::send_due_poll_replies(time_point now) noexcept {
    auto iterator = pending_poll_replies_.begin();
    while (iterator != pending_poll_replies_.end()) {
        if (iterator->due > now) {
            ++iterator;
            continue;
        }

        config_.poll_reply_identity.poll_reply_counter = static_cast<std::uint16_t>(
            (static_cast<unsigned int>(config_.poll_reply_identity.poll_reply_counter) + 1U) & 0xffffU);
        auto reply_identity = config_.poll_reply_identity;
        reply_identity.ip = iterator->local_ip;
        reply_identity.artnet_output_active =
            artnet_output_active_.load(std::memory_order_acquire);
        const auto reply = build_art_poll_reply(core_.port_address(), reply_identity);
        if (!reply.has_value()) {
            ++diagnostics_.poll_replies_not_built;
            iterator = pending_poll_replies_.erase(iterator);
            continue;
        }

        if (!transport_.send_to(iterator->destination, kArtNetUdpPort, reply->bytes)) {
            ++diagnostics_.send_errors;
            iterator = pending_poll_replies_.erase(iterator);
            handle_transport_failure(now, false);
            return;
        }

        ++diagnostics_.poll_replies_sent;
        iterator = pending_poll_replies_.erase(iterator);
    }
}

void ArtNetRuntime::handle_transport_failure(
    time_point now,
    bool receive_failure) noexcept {
    if (receive_failure) {
        ++diagnostics_.receive_errors;
    }
    transport_failure_seen_ = true;
    clear_pending_poll_replies();
    if (transport_.is_open()) {
        transport_.close();
    }
    diagnostics_.transport_open = false;
    next_bind_attempt_ = now + config_.rebind_delay;
}

void ArtNetRuntime::clear_pending_poll_replies() noexcept {
    diagnostics_.poll_replies_dropped += static_cast<std::uint64_t>(pending_poll_replies_.size());
    pending_poll_replies_.clear();
}

std::chrono::milliseconds ArtNetRuntime::next_poll_reply_delay() noexcept {
    const auto requested = delay_source_.next_delay();
    const auto minimum = std::chrono::milliseconds{0};
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
        kArtNetPollReplyMaximumDelay);
    const auto clamped = std::clamp(requested, minimum, maximum);
    if (clamped != requested) {
        ++diagnostics_.delay_values_clamped;
    }
    return clamped;
}

}  // namespace dmxwb
