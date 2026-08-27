#pragma once

#include "dmxwb/dmx_snapshot.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace dmxwb {

inline constexpr std::uint16_t kArtNetProtocolRevision = 14;
inline constexpr std::uint16_t kArtNetUdpPort = 0x1936;
inline constexpr std::uint16_t kArtNetPortAddressMax = 0x7fff;
inline constexpr std::uint16_t kArtNetOpPoll = 0x2000;
inline constexpr std::uint16_t kArtNetOpPollReply = 0x2100;
inline constexpr std::uint16_t kArtNetOpDmx = 0x5000;
inline constexpr std::uint16_t kArtNetOpSync = 0x5200;
inline constexpr std::size_t kArtNetPollReplyPacketSize = 239;
inline constexpr auto kArtNetSourceLossTimeout = std::chrono::seconds{3};
inline constexpr auto kArtNetSyncTimeout = std::chrono::seconds{4};

struct ArtNetIpv4Address final {
    std::array<std::uint8_t, 4> octets{};

    [[nodiscard]] friend constexpr bool operator==(
        const ArtNetIpv4Address&,
        const ArtNetIpv4Address&) noexcept = default;
};

struct ArtNetSource final {
    ArtNetIpv4Address ip{};
    std::uint8_t physical{0};

    [[nodiscard]] friend constexpr bool operator==(
        const ArtNetSource&,
        const ArtNetSource&) noexcept = default;
};

enum class ArtNetSourceState {
    waiting,
    active,
    lost,
    conflict,
};

enum class ArtNetSyncMode {
    asynchronous,
    synchronous,
};

enum class ArtNetParseError {
    none,
    packet_too_short,
    invalid_id,
    protocol_revision_too_old,
    invalid_dmx_length,
    truncated_dmx_payload,
};

enum class ArtNetAction {
    rejected,
    ignored_unsupported_opcode,
    ignored_wrong_port_address,
    ignored_targeted_poll,
    ignored_sync_source,
    ignored_stale_sequence,
    conflict,
    poll_reply_requested,
    dmx_committed,
    dmx_staged,
    sync_entered,
    sync_committed,
    sync_no_change,
    sync_timeout_async,
    sync_timeout_committed,
    source_lost,
    no_change,
};

struct ArtNetProcessResult final {
    ArtNetAction action{ArtNetAction::no_change};
    ArtNetParseError error{ArtNetParseError::none};

    [[nodiscard]] bool rejected() const noexcept {
        return action == ArtNetAction::rejected;
    }
};

struct ArtNetCoreConfig final {
    std::uint16_t port_address{0};
};

struct ArtNetPollReplyIdentity final {
    ArtNetIpv4Address ip{};
    std::array<std::uint8_t, 6> mac{};
    std::optional<std::uint16_t> oem_code;
    std::uint16_t firmware_version{0};
    std::uint16_t poll_reply_counter{0};
    std::string port_name{"DMXWB"};
    std::string long_name{"DMXWB Art-Net output"};
    bool artnet_output_active{false};
};

struct ArtNetPollReply final {
    std::array<std::uint8_t, kArtNetPollReplyPacketSize> bytes{};
};

[[nodiscard]] std::optional<ArtNetPollReply> build_art_poll_reply(
    std::uint16_t port_address,
    const ArtNetPollReplyIdentity& identity) noexcept;

class ArtNetCore final {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;
    using ChannelData = std::array<std::uint8_t, kDmxMaxChannels>;

    [[nodiscard]] static std::optional<ArtNetCore> create(ArtNetCoreConfig config) noexcept;

    [[nodiscard]] ArtNetProcessResult process_datagram(
        std::span<const std::uint8_t> packet,
        ArtNetIpv4Address source_ip,
        time_point now) noexcept;

    [[nodiscard]] ArtNetProcessResult tick(time_point now) noexcept;

    [[nodiscard]] std::uint16_t port_address() const noexcept;
    [[nodiscard]] bool legacy_zero_port_address() const noexcept;
    [[nodiscard]] ArtNetSourceState source_state() const noexcept;
    [[nodiscard]] ArtNetSyncMode sync_mode() const noexcept;
    [[nodiscard]] std::optional<ArtNetSource> active_source() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> last_sequence() const noexcept;
    [[nodiscard]] bool has_committed_dmx() const noexcept;
    [[nodiscard]] std::uint64_t committed_revision() const noexcept;
    [[nodiscard]] std::uint8_t channel(std::size_t one_based_channel) const noexcept;
    [[nodiscard]] const ChannelData& committed_state() const noexcept;

    [[nodiscard]] std::shared_ptr<const DmxSnapshot> build_physical_snapshot(
        DmxSnapshot::Generation generation) const;

private:
    explicit ArtNetCore(ArtNetCoreConfig config) noexcept;

    [[nodiscard]] ArtNetProcessResult process_art_dmx(
        std::span<const std::uint8_t> packet,
        ArtNetIpv4Address source_ip,
        time_point now) noexcept;
    [[nodiscard]] ArtNetProcessResult process_art_poll(
        std::span<const std::uint8_t> packet) const noexcept;
    [[nodiscard]] ArtNetProcessResult process_art_sync(
        std::span<const std::uint8_t> packet,
        ArtNetIpv4Address source_ip,
        time_point now) noexcept;

    [[nodiscard]] static bool has_artnet_id(std::span<const std::uint8_t> packet) noexcept;
    [[nodiscard]] static std::uint16_t read_le16(std::span<const std::uint8_t> packet, std::size_t offset) noexcept;
    [[nodiscard]] static std::uint16_t read_be16(std::span<const std::uint8_t> packet, std::size_t offset) noexcept;
    [[nodiscard]] static std::uint8_t byte_or_zero(std::span<const std::uint8_t> packet, std::size_t offset) noexcept;
    [[nodiscard]] static bool sequence_is_newer(std::uint8_t candidate, std::uint8_t previous) noexcept;

    void reset_source_tracking() noexcept;
    void commit_staging() noexcept;

    ArtNetCoreConfig config_{};
    ChannelData committed_state_{};
    ChannelData staging_state_{};
    bool has_committed_dmx_{false};
    bool staging_dirty_{false};
    std::uint64_t committed_revision_{0};
    ArtNetSourceState source_state_{ArtNetSourceState::waiting};
    ArtNetSyncMode sync_mode_{ArtNetSyncMode::asynchronous};
    std::optional<ArtNetSource> active_source_;
    std::optional<std::uint8_t> last_sequence_;
    std::optional<time_point> last_source_dmx_;
    std::optional<time_point> last_sync_;
};

}  // namespace dmxwb
