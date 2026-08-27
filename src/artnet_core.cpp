#include "dmxwb/artnet_core.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dmxwb {
namespace {

constexpr std::array<std::uint8_t, 8> kArtNetId{
    static_cast<std::uint8_t>('A'),
    static_cast<std::uint8_t>('r'),
    static_cast<std::uint8_t>('t'),
    static_cast<std::uint8_t>('-'),
    static_cast<std::uint8_t>('N'),
    static_cast<std::uint8_t>('e'),
    static_cast<std::uint8_t>('t'),
    0x00};

constexpr std::size_t kHeaderSize = 10;
constexpr std::size_t kArtPollMinimumSize = 14;
constexpr std::size_t kArtSyncMinimumSize = 14;
constexpr std::size_t kArtDmxHeaderSize = 18;

constexpr std::size_t kPollReplyIpOffset = 10;
constexpr std::size_t kPollReplyPortOffset = 14;
constexpr std::size_t kPollReplyVersInfoOffset = 16;
constexpr std::size_t kPollReplyNetSwitchOffset = 18;
constexpr std::size_t kPollReplySubSwitchOffset = 19;
constexpr std::size_t kPollReplyOemOffset = 20;
constexpr std::size_t kPollReplyPortNameOffset = 26;
constexpr std::size_t kPollReplyPortNameSize = 18;
constexpr std::size_t kPollReplyLongNameOffset = 44;
constexpr std::size_t kPollReplyLongNameSize = 64;
constexpr std::size_t kPollReplyNodeReportOffset = 108;
constexpr std::size_t kPollReplyNodeReportSize = 64;
constexpr std::size_t kPollReplyNumPortsOffset = 172;
constexpr std::size_t kPollReplyPortTypesOffset = 174;
constexpr std::size_t kPollReplyGoodOutputAOffset = 182;
constexpr std::size_t kPollReplySwOutOffset = 190;
constexpr std::size_t kPollReplyStyleOffset = 200;
constexpr std::size_t kPollReplyMacOffset = 201;
constexpr std::size_t kPollReplyBindIpOffset = 207;
constexpr std::size_t kPollReplyBindIndexOffset = 211;
constexpr std::size_t kPollReplyStatus2Offset = 212;
constexpr std::size_t kPollReplyGoodOutputBOffset = 213;
constexpr std::size_t kPollReplyStatus3Offset = 217;
constexpr std::size_t kPollReplyRefreshRateOffset = 226;

constexpr std::uint8_t kPortTypeOutputDmx512 = 0x80;
constexpr std::uint8_t kGoodOutputDataActive = 0x80;
constexpr std::uint8_t kGoodOutputBRdmDisabledContinuous = 0xc0;
constexpr std::uint8_t kStatus2Supports15BitPortAddress = 0x08;
constexpr std::uint8_t kStyleNode = 0x00;

void write_le16(std::array<std::uint8_t, kArtNetPollReplyPacketSize>& bytes,
                std::size_t offset,
                std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void write_be16(std::array<std::uint8_t, kArtNetPollReplyPacketSize>& bytes,
                std::size_t offset,
                std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>(value & 0xffU);
}

void write_fixed_string(
    std::array<std::uint8_t, kArtNetPollReplyPacketSize>& bytes,
    std::size_t offset,
    std::size_t field_size,
    std::string_view value) noexcept {
    if (field_size == 0) {
        return;
    }
    const auto copy_size = std::min(value.size(), field_size - 1);
    for (std::size_t index = 0; index < copy_size; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value[index]);
    }
}

void write_node_report(
    std::array<std::uint8_t, kArtNetPollReplyPacketSize>& bytes,
    std::uint16_t counter) noexcept {
    std::array<char, 19> report{
        '#', '0', '0', '0', '1', ' ', '[', '0', '0', '0', '0', ']', ' ',
        'D', 'M', 'X', 'W', 'B', '\0'};
    auto value = static_cast<unsigned int>(counter % 10000U);
    for (std::size_t index = 0; index < 4; ++index) {
        const auto position = 10U - index;
        report[position] = static_cast<char>('0' + static_cast<char>(value % 10U));
        value /= 10U;
    }
    write_fixed_string(
        bytes,
        kPollReplyNodeReportOffset,
        kPollReplyNodeReportSize,
        std::string_view{report.data(), report.size() - 1});
}

}  // namespace

std::optional<ArtNetPollReply> build_art_poll_reply(
    std::uint16_t port_address,
    const ArtNetPollReplyIdentity& identity) noexcept {
    if (port_address > kArtNetPortAddressMax || !identity.oem_code.has_value()) {
        return std::nullopt;
    }

    ArtNetPollReply reply;
    std::copy(kArtNetId.begin(), kArtNetId.end(), reply.bytes.begin());
    write_le16(reply.bytes, 8, kArtNetOpPollReply);
    std::copy(identity.ip.octets.begin(), identity.ip.octets.end(), reply.bytes.begin() + kPollReplyIpOffset);
    write_le16(reply.bytes, kPollReplyPortOffset, kArtNetUdpPort);
    write_be16(reply.bytes, kPollReplyVersInfoOffset, identity.firmware_version);
    reply.bytes[kPollReplyNetSwitchOffset] = static_cast<std::uint8_t>((port_address >> 8U) & 0x7fU);
    reply.bytes[kPollReplySubSwitchOffset] = static_cast<std::uint8_t>((port_address >> 4U) & 0x0fU);
    write_be16(reply.bytes, kPollReplyOemOffset, *identity.oem_code);
    write_fixed_string(reply.bytes, kPollReplyPortNameOffset, kPollReplyPortNameSize, identity.port_name);
    write_fixed_string(reply.bytes, kPollReplyLongNameOffset, kPollReplyLongNameSize, identity.long_name);
    write_node_report(reply.bytes, identity.poll_reply_counter);
    write_be16(reply.bytes, kPollReplyNumPortsOffset, 1);
    reply.bytes[kPollReplyPortTypesOffset] = kPortTypeOutputDmx512;
    reply.bytes[kPollReplyGoodOutputAOffset] = identity.artnet_output_active ? kGoodOutputDataActive : 0x00;
    reply.bytes[kPollReplySwOutOffset] = static_cast<std::uint8_t>(port_address & 0x0fU);
    reply.bytes[kPollReplyStyleOffset] = kStyleNode;
    std::copy(identity.mac.begin(), identity.mac.end(), reply.bytes.begin() + kPollReplyMacOffset);
    std::copy(identity.ip.octets.begin(), identity.ip.octets.end(), reply.bytes.begin() + kPollReplyBindIpOffset);
    reply.bytes[kPollReplyBindIndexOffset] = 1;
    reply.bytes[kPollReplyStatus2Offset] = kStatus2Supports15BitPortAddress;
    reply.bytes[kPollReplyGoodOutputBOffset] = kGoodOutputBRdmDisabledContinuous;
    reply.bytes[kPollReplyStatus3Offset] = 0x00;  // Hold Last failsafe.
    write_be16(reply.bytes, kPollReplyRefreshRateOffset, 44);
    return reply;
}

std::optional<ArtNetCore> ArtNetCore::create(ArtNetCoreConfig config) noexcept {
    if (config.port_address > kArtNetPortAddressMax) {
        return std::nullopt;
    }
    return ArtNetCore{config};
}

ArtNetCore::ArtNetCore(ArtNetCoreConfig config) noexcept
    : config_(config) {}

ArtNetProcessResult ArtNetCore::process_datagram(
    std::span<const std::uint8_t> packet,
    ArtNetIpv4Address source_ip,
    time_point now) noexcept {
    if (packet.size() < kHeaderSize) {
        return {ArtNetAction::rejected, ArtNetParseError::packet_too_short};
    }
    if (!has_artnet_id(packet)) {
        return {ArtNetAction::rejected, ArtNetParseError::invalid_id};
    }

    const auto opcode = read_le16(packet, 8);
    switch (opcode) {
        case kArtNetOpDmx:
            return process_art_dmx(packet, source_ip, now);
        case kArtNetOpPoll:
            return process_art_poll(packet);
        case kArtNetOpSync:
            return process_art_sync(packet, source_ip, now);
        default:
            return {ArtNetAction::ignored_unsupported_opcode, ArtNetParseError::none};
    }
}

ArtNetProcessResult ArtNetCore::process_art_dmx(
    std::span<const std::uint8_t> packet,
    ArtNetIpv4Address source_ip,
    time_point now) noexcept {
    if (packet.size() < kArtDmxHeaderSize) {
        return {ArtNetAction::rejected, ArtNetParseError::packet_too_short};
    }
    if (read_be16(packet, 10) < kArtNetProtocolRevision) {
        return {ArtNetAction::rejected, ArtNetParseError::protocol_revision_too_old};
    }

    const auto dmx_length = read_be16(packet, 16);
    if (dmx_length < 2 || dmx_length > kDmxMaxChannels || (dmx_length % 2U) != 0U) {
        return {ArtNetAction::rejected, ArtNetParseError::invalid_dmx_length};
    }
    if (packet.size() < kArtDmxHeaderSize + static_cast<std::size_t>(dmx_length)) {
        return {ArtNetAction::rejected, ArtNetParseError::truncated_dmx_payload};
    }

    const auto packet_port_address = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[15] & 0x7fU) << 8U) |
        static_cast<std::uint16_t>(packet[14]));
    if (packet_port_address != config_.port_address) {
        return {ArtNetAction::ignored_wrong_port_address, ArtNetParseError::none};
    }

    const ArtNetSource candidate{source_ip, packet[13]};
    if (!active_source_.has_value()) {
        active_source_ = candidate;
        source_state_ = ArtNetSourceState::active;
        last_sequence_.reset();
        staging_state_ = committed_state_;
        staging_dirty_ = false;
    } else if (*active_source_ != candidate) {
        source_state_ = ArtNetSourceState::conflict;
        return {ArtNetAction::conflict, ArtNetParseError::none};
    }

    last_source_dmx_ = now;
    source_state_ = ArtNetSourceState::active;

    const auto sequence = packet[12];
    if (sequence == 0) {
        last_sequence_.reset();
    } else if (!last_sequence_.has_value()) {
        last_sequence_ = sequence;
    } else if (sequence_is_newer(sequence, *last_sequence_)) {
        last_sequence_ = sequence;
    } else {
        return {ArtNetAction::ignored_stale_sequence, ArtNetParseError::none};
    }

    auto& target = sync_mode_ == ArtNetSyncMode::synchronous ? staging_state_ : committed_state_;
    std::copy_n(packet.begin() + static_cast<std::ptrdiff_t>(kArtDmxHeaderSize), dmx_length, target.begin());

    if (sync_mode_ == ArtNetSyncMode::synchronous) {
        staging_dirty_ = true;
        return {ArtNetAction::dmx_staged, ArtNetParseError::none};
    }

    staging_state_ = committed_state_;
    staging_dirty_ = false;
    has_committed_dmx_ = true;
    ++committed_revision_;
    return {ArtNetAction::dmx_committed, ArtNetParseError::none};
}

ArtNetProcessResult ArtNetCore::process_art_poll(std::span<const std::uint8_t> packet) const noexcept {
    if (packet.size() < kArtPollMinimumSize) {
        return {ArtNetAction::rejected, ArtNetParseError::packet_too_short};
    }
    if (read_be16(packet, 10) < kArtNetProtocolRevision) {
        return {ArtNetAction::rejected, ArtNetParseError::protocol_revision_too_old};
    }

    const bool targeted_mode = (packet[12] & 0x20U) != 0U;
    if (!targeted_mode) {
        return {ArtNetAction::poll_reply_requested, ArtNetParseError::none};
    }

    const auto top = static_cast<std::uint16_t>(
        ((static_cast<std::uint16_t>(byte_or_zero(packet, 14)) << 8U) |
          static_cast<std::uint16_t>(byte_or_zero(packet, 15))) &
         kArtNetPortAddressMax);
    const auto bottom = static_cast<std::uint16_t>(
        ((static_cast<std::uint16_t>(byte_or_zero(packet, 16)) << 8U) |
          static_cast<std::uint16_t>(byte_or_zero(packet, 17))) &
         kArtNetPortAddressMax);

    if (bottom > top || config_.port_address < bottom || config_.port_address > top) {
        return {ArtNetAction::ignored_targeted_poll, ArtNetParseError::none};
    }
    return {ArtNetAction::poll_reply_requested, ArtNetParseError::none};
}

ArtNetProcessResult ArtNetCore::process_art_sync(
    std::span<const std::uint8_t> packet,
    ArtNetIpv4Address source_ip,
    time_point now) noexcept {
    if (packet.size() < kArtSyncMinimumSize) {
        return {ArtNetAction::rejected, ArtNetParseError::packet_too_short};
    }
    if (read_be16(packet, 10) < kArtNetProtocolRevision) {
        return {ArtNetAction::rejected, ArtNetParseError::protocol_revision_too_old};
    }
    if (!active_source_.has_value() || active_source_->ip != source_ip || source_state_ == ArtNetSourceState::conflict) {
        return {ArtNetAction::ignored_sync_source, ArtNetParseError::none};
    }

    last_sync_ = now;
    if (sync_mode_ == ArtNetSyncMode::asynchronous) {
        sync_mode_ = ArtNetSyncMode::synchronous;
        staging_state_ = committed_state_;
        staging_dirty_ = false;
        return {ArtNetAction::sync_entered, ArtNetParseError::none};
    }

    if (!staging_dirty_) {
        return {ArtNetAction::sync_no_change, ArtNetParseError::none};
    }
    commit_staging();
    return {ArtNetAction::sync_committed, ArtNetParseError::none};
}

ArtNetProcessResult ArtNetCore::tick(time_point now) noexcept {
    if (active_source_.has_value() && last_source_dmx_.has_value() && now >= *last_source_dmx_ &&
        now - *last_source_dmx_ >= kArtNetSourceLossTimeout) {
        source_state_ = ArtNetSourceState::lost;
        reset_source_tracking();
        return {ArtNetAction::source_lost, ArtNetParseError::none};
    }

    if (sync_mode_ == ArtNetSyncMode::synchronous && last_sync_.has_value() && now >= *last_sync_ &&
        now - *last_sync_ >= kArtNetSyncTimeout) {
        sync_mode_ = ArtNetSyncMode::asynchronous;
        last_sync_.reset();
        if (staging_dirty_) {
            commit_staging();
            return {ArtNetAction::sync_timeout_committed, ArtNetParseError::none};
        }
        return {ArtNetAction::sync_timeout_async, ArtNetParseError::none};
    }

    return {ArtNetAction::no_change, ArtNetParseError::none};
}

std::uint16_t ArtNetCore::port_address() const noexcept {
    return config_.port_address;
}

bool ArtNetCore::legacy_zero_port_address() const noexcept {
    return config_.port_address == 0;
}

ArtNetSourceState ArtNetCore::source_state() const noexcept {
    return source_state_;
}

ArtNetSyncMode ArtNetCore::sync_mode() const noexcept {
    return sync_mode_;
}

std::optional<ArtNetSource> ArtNetCore::active_source() const noexcept {
    return active_source_;
}

std::optional<std::uint8_t> ArtNetCore::last_sequence() const noexcept {
    return last_sequence_;
}

bool ArtNetCore::has_committed_dmx() const noexcept {
    return has_committed_dmx_;
}

std::uint64_t ArtNetCore::committed_revision() const noexcept {
    return committed_revision_;
}

std::uint8_t ArtNetCore::channel(std::size_t one_based_channel) const noexcept {
    if (!is_valid_dmx_channel(one_based_channel)) {
        return 0;
    }
    return committed_state_[one_based_channel - 1];
}

const ArtNetCore::ChannelData& ArtNetCore::committed_state() const noexcept {
    return committed_state_;
}

std::shared_ptr<const DmxSnapshot> ArtNetCore::build_physical_snapshot(
    DmxSnapshot::Generation generation) const {
    if (!has_committed_dmx_) {
        return {};
    }
    const auto maybe_builder = DmxSnapshotBuilder::create(kDmxPhysicalMaxSlots);
    if (!maybe_builder.has_value()) {
        return {};
    }
    auto builder = *maybe_builder;
    for (std::size_t channel_number = 1; channel_number <= kDmxPhysicalMaxSlots; ++channel_number) {
        if (!builder.set_channel(channel_number, committed_state_[channel_number - 1])) {
            return {};
        }
    }
    return builder.build(generation);
}

bool ArtNetCore::has_artnet_id(std::span<const std::uint8_t> packet) noexcept {
    return packet.size() >= kArtNetId.size() &&
        std::equal(kArtNetId.begin(), kArtNetId.end(), packet.begin());
}

std::uint16_t ArtNetCore::read_le16(std::span<const std::uint8_t> packet, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(packet[offset]) |
        (static_cast<std::uint16_t>(packet[offset + 1]) << 8U));
}

std::uint16_t ArtNetCore::read_be16(std::span<const std::uint8_t> packet, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[offset]) << 8U) |
        static_cast<std::uint16_t>(packet[offset + 1]));
}

std::uint8_t ArtNetCore::byte_or_zero(std::span<const std::uint8_t> packet, std::size_t offset) noexcept {
    return offset < packet.size() ? packet[offset] : std::uint8_t{0};
}

bool ArtNetCore::sequence_is_newer(std::uint8_t candidate, std::uint8_t previous) noexcept {
    if (candidate == 0 || previous == 0 || candidate == previous) {
        return false;
    }
    constexpr unsigned int kSequenceRing = 255U;
    constexpr unsigned int kForwardWindow = 127U;
    const auto candidate_index = static_cast<unsigned int>(candidate - 1U);
    const auto previous_index = static_cast<unsigned int>(previous - 1U);
    const auto delta = (candidate_index + kSequenceRing - previous_index) % kSequenceRing;
    return delta >= 1U && delta <= kForwardWindow;
}

void ArtNetCore::reset_source_tracking() noexcept {
    active_source_.reset();
    last_sequence_.reset();
    last_source_dmx_.reset();
    last_sync_.reset();
    sync_mode_ = ArtNetSyncMode::asynchronous;
    staging_state_ = committed_state_;
    staging_dirty_ = false;
}

void ArtNetCore::commit_staging() noexcept {
    committed_state_ = staging_state_;
    staging_dirty_ = false;
    has_committed_dmx_ = true;
    ++committed_revision_;
}

}  // namespace dmxwb
