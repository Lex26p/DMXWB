#include "dmxwb/artnet_core.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect_true(bool condition, std::string_view name) {
    if (condition) {
        std::cout << "[PASS] " << name << '\n';
    } else {
        ++failures;
        std::cerr << "[FAIL] " << name << '\n';
    }
}

constexpr dmxwb::ArtNetIpv4Address kIpA{{10, 1, 2, 3}};
constexpr dmxwb::ArtNetIpv4Address kIpB{{10, 1, 2, 4}};

void append_artnet_header(std::vector<std::uint8_t>& packet, std::uint16_t opcode) {
    const std::array<std::uint8_t, 8> id{
        static_cast<std::uint8_t>('A'),
        static_cast<std::uint8_t>('r'),
        static_cast<std::uint8_t>('t'),
        static_cast<std::uint8_t>('-'),
        static_cast<std::uint8_t>('N'),
        static_cast<std::uint8_t>('e'),
        static_cast<std::uint8_t>('t'),
        0};
    packet.insert(packet.end(), id.begin(), id.end());
    packet.push_back(static_cast<std::uint8_t>(opcode & 0xffU));
    packet.push_back(static_cast<std::uint8_t>((opcode >> 8U) & 0xffU));
}

void append_protocol_revision(std::vector<std::uint8_t>& packet, std::uint16_t revision) {
    packet.push_back(static_cast<std::uint8_t>((revision >> 8U) & 0xffU));
    packet.push_back(static_cast<std::uint8_t>(revision & 0xffU));
}

std::vector<std::uint8_t> make_dmx(
    std::uint16_t port_address,
    std::uint8_t physical,
    std::uint8_t sequence,
    std::span<const std::uint8_t> data,
    std::uint16_t protocol_revision = dmxwb::kArtNetProtocolRevision) {
    std::vector<std::uint8_t> packet;
    packet.reserve(18 + data.size());
    append_artnet_header(packet, dmxwb::kArtNetOpDmx);
    append_protocol_revision(packet, protocol_revision);
    packet.push_back(sequence);
    packet.push_back(physical);
    packet.push_back(static_cast<std::uint8_t>(port_address & 0xffU));
    packet.push_back(static_cast<std::uint8_t>((port_address >> 8U) & 0x7fU));
    const auto length = static_cast<std::uint16_t>(data.size());
    packet.push_back(static_cast<std::uint8_t>((length >> 8U) & 0xffU));
    packet.push_back(static_cast<std::uint8_t>(length & 0xffU));
    packet.insert(packet.end(), data.begin(), data.end());
    return packet;
}

std::vector<std::uint8_t> make_sync(
    std::uint16_t protocol_revision = dmxwb::kArtNetProtocolRevision) {
    std::vector<std::uint8_t> packet;
    packet.reserve(14);
    append_artnet_header(packet, dmxwb::kArtNetOpSync);
    append_protocol_revision(packet, protocol_revision);
    packet.push_back(0);
    packet.push_back(0);
    return packet;
}

std::vector<std::uint8_t> make_poll(
    bool targeted,
    std::optional<std::uint16_t> bottom = std::nullopt,
    std::optional<std::uint16_t> top = std::nullopt,
    std::uint16_t protocol_revision = dmxwb::kArtNetProtocolRevision) {
    std::vector<std::uint8_t> packet;
    packet.reserve(bottom.has_value() || top.has_value() ? 18 : 14);
    append_artnet_header(packet, dmxwb::kArtNetOpPoll);
    append_protocol_revision(packet, protocol_revision);
    packet.push_back(targeted ? 0x20 : 0x00);
    packet.push_back(0);
    if (bottom.has_value() || top.has_value()) {
        const auto resolved_top = top.value_or(0);
        const auto resolved_bottom = bottom.value_or(0);
        packet.push_back(static_cast<std::uint8_t>((resolved_top >> 8U) & 0xffU));
        packet.push_back(static_cast<std::uint8_t>(resolved_top & 0xffU));
        packet.push_back(static_cast<std::uint8_t>((resolved_bottom >> 8U) & 0xffU));
        packet.push_back(static_cast<std::uint8_t>(resolved_bottom & 0xffU));
    }
    return packet;
}

std::array<std::uint8_t, 512> make_pattern(std::uint8_t base) {
    std::array<std::uint8_t, 512> data{};
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<std::uint8_t>((static_cast<unsigned int>(base) + index) % 251U);
    }
    return data;
}

void test_core_configuration_and_header_validation() {
    expect_true(dmxwb::ArtNetCore::create({0}).has_value(), "Port-Address 0 accepted as compatibility configuration");
    expect_true(dmxwb::ArtNetCore::create({32767}).has_value(), "maximum 15-bit Port-Address accepted");
    expect_true(!dmxwb::ArtNetCore::create({32768}).has_value(), "Port-Address above 15-bit range rejected");

    auto core = *dmxwb::ArtNetCore::create({0});
    expect_true(core.legacy_zero_port_address(), "Port-Address 0 is explicitly detectable as legacy compatibility");
    expect_true(!core.build_physical_snapshot(1), "no physical Art-Net snapshot exists before first valid ArtDmx");

    const auto t0 = dmxwb::ArtNetCore::time_point{};
    std::vector<std::uint8_t> too_short(9, 0);
    auto result = core.process_datagram(too_short, kIpA, t0);
    expect_true(result.rejected() && result.error == dmxwb::ArtNetParseError::packet_too_short,
        "packet shorter than common Art-Net header rejected");

    auto bad_id = make_sync();
    bad_id[0] = static_cast<std::uint8_t>('X');
    result = core.process_datagram(bad_id, kIpA, t0);
    expect_true(result.rejected() && result.error == dmxwb::ArtNetParseError::invalid_id,
        "invalid Art-Net ID rejected");

    std::vector<std::uint8_t> unknown;
    append_artnet_header(unknown, 0x1234);
    result = core.process_datagram(unknown, kIpA, t0);
    expect_true(result.action == dmxwb::ArtNetAction::ignored_unsupported_opcode,
        "unknown opcode ignored after valid common header");
}

void test_art_dmx_validation_trailing_and_port_address() {
    auto core = *dmxwb::ArtNetCore::create({0x1234});
    const auto t0 = dmxwb::ArtNetCore::time_point{};
    const std::array<std::uint8_t, 2> two{{11, 22}};

    auto old_version = make_dmx(0x1234, 1, 1, two, 13);
    auto result = core.process_datagram(old_version, kIpA, t0);
    expect_true(result.rejected() && result.error == dmxwb::ArtNetParseError::protocol_revision_too_old,
        "ArtDmx protocol revision below 14 rejected");

    auto wrong_port = make_dmx(0x1235, 1, 1, two);
    result = core.process_datagram(wrong_port, kIpA, t0);
    expect_true(result.action == dmxwb::ArtNetAction::ignored_wrong_port_address,
        "ArtDmx for another Port-Address ignored");
    expect_true(core.source_state() == dmxwb::ArtNetSourceState::waiting,
        "wrong-universe ArtDmx does not acquire source lock");

    auto odd = make_dmx(0x1234, 1, 1, std::array<std::uint8_t, 3>{1, 2, 3});
    result = core.process_datagram(odd, kIpA, t0);
    expect_true(result.rejected() && result.error == dmxwb::ArtNetParseError::invalid_dmx_length,
        "odd ArtDmx Length rejected");

    auto zero_length = make_dmx(0x1234, 1, 1, std::span<const std::uint8_t>{});
    result = core.process_datagram(zero_length, kIpA, t0);
    expect_true(result.rejected() && result.error == dmxwb::ArtNetParseError::invalid_dmx_length,
        "ArtDmx Length below 2 rejected");

    std::vector<std::uint8_t> too_long_data(514, 1);
    auto too_long = make_dmx(0x1234, 1, 1, too_long_data);
    result = core.process_datagram(too_long, kIpA, t0);
    expect_true(result.rejected() && result.error == dmxwb::ArtNetParseError::invalid_dmx_length,
        "ArtDmx Length above 512 rejected");

    auto truncated = make_dmx(0x1234, 1, 1, std::array<std::uint8_t, 4>{1, 2, 3, 4});
    truncated.pop_back();
    result = core.process_datagram(truncated, kIpA, t0);
    expect_true(result.rejected() && result.error == dmxwb::ArtNetParseError::truncated_dmx_payload,
        "truncated ArtDmx payload rejected");

    auto trailing = make_dmx(0x1234, 1, 1, two);
    trailing.push_back(0xaa);
    trailing.push_back(0xbb);
    result = core.process_datagram(trailing, kIpA, t0);
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed,
        "valid trailing ArtDmx extension bytes ignored");
    expect_true(core.channel(1) == 11 && core.channel(2) == 22,
        "mandatory ArtDmx bytes applied despite trailing extension");

    auto unused_net_bit = make_dmx(0x1234, 1, 2, two);
    unused_net_bit[15] = static_cast<std::uint8_t>(unused_net_bit[15] | 0x80U);
    result = core.process_datagram(unused_net_bit, kIpA, t0 + std::chrono::milliseconds{1});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed,
        "unused high Net bit is not treated as a Port-Address mismatch");
}

void test_persistent_512_state_and_300_projection() {
    auto core = *dmxwb::ArtNetCore::create({1});
    const auto t0 = dmxwb::ArtNetCore::time_point{};
    const auto full = make_pattern(7);
    auto result = core.process_datagram(make_dmx(1, 2, 1, full), kIpA, t0);
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed, "512-channel ArtDmx accepted");
    expect_true(core.channel(1) == full[0] && core.channel(300) == full[299] && core.channel(512) == full[511],
        "persistent Art-Net state retains all 512 network channels");

    const auto snapshot = core.build_physical_snapshot(42);
    expect_true(snapshot != nullptr && snapshot->slot_count() == 300,
        "physical projection contains exactly first 300 DMX slots");
    if (snapshot) {
        expect_true(snapshot->generation() == 42, "physical projection preserves requested generation");
        expect_true(snapshot->active_channels().size() == 300, "physical active channel span is limited to 300");
        expect_true(snapshot->channel(300) == std::optional<std::uint8_t>{full[299]},
            "network channel 300 reaches physical projection");
    }

    const std::array<std::uint8_t, 2> short_data{{201, 202}};
    result = core.process_datagram(make_dmx(1, 2, 2, short_data), kIpA, t0 + std::chrono::milliseconds{1});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed, "short valid ArtDmx accepted");
    expect_true(core.channel(1) == 201 && core.channel(2) == 202,
        "short ArtDmx updates channels inside Length");
    expect_true(core.channel(3) == full[2] && core.channel(512) == full[511],
        "short ArtDmx holds persistent channels beyond Length");
}

void test_sequence_rollover_stale_and_zero_disable() {
    auto core = *dmxwb::ArtNetCore::create({2});
    const auto t0 = dmxwb::ArtNetCore::time_point{};
    const std::array<std::uint8_t, 2> a{{10, 10}};
    const std::array<std::uint8_t, 2> b{{20, 20}};
    const std::array<std::uint8_t, 2> c{{30, 30}};
    const std::array<std::uint8_t, 2> d{{40, 40}};

    expect_true(core.process_datagram(make_dmx(2, 1, 254, a), kIpA, t0).action == dmxwb::ArtNetAction::dmx_committed,
        "first non-zero sequence accepted as baseline");
    expect_true(core.process_datagram(make_dmx(2, 1, 255, b), kIpA, t0 + std::chrono::milliseconds{1}).action == dmxwb::ArtNetAction::dmx_committed,
        "sequence 254 to 255 accepted");
    expect_true(core.process_datagram(make_dmx(2, 1, 1, c), kIpA, t0 + std::chrono::milliseconds{2}).action == dmxwb::ArtNetAction::dmx_committed,
        "sequence FF to 01 rollover accepted");

    const auto revision_before_stale = core.committed_revision();
    auto result = core.process_datagram(make_dmx(2, 1, 255, d), kIpA, t0 + std::chrono::milliseconds{3});
    expect_true(result.action == dmxwb::ArtNetAction::ignored_stale_sequence,
        "stale pre-rollover sequence ignored");
    expect_true(core.committed_revision() == revision_before_stale && core.channel(1) == 30,
        "stale sequence cannot replace committed state");

    result = core.process_datagram(make_dmx(2, 1, 0, d), kIpA, t0 + std::chrono::milliseconds{4});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed && !core.last_sequence().has_value(),
        "Sequence 0 disables ordering and clears sequence baseline");
    result = core.process_datagram(make_dmx(2, 1, 250, a), kIpA, t0 + std::chrono::milliseconds{5});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed && core.last_sequence() == std::optional<std::uint8_t>{250},
        "first non-zero packet after Sequence 0 establishes a fresh baseline");

    auto gap_core = *dmxwb::ArtNetCore::create({22});
    expect_true(gap_core.process_datagram(make_dmx(22, 1, 1, a), kIpA, t0).action == dmxwb::ArtNetAction::dmx_committed,
        "sequence gap test baseline accepted");
    expect_true(gap_core.process_datagram(make_dmx(22, 1, 10, b), kIpA, t0 + std::chrono::milliseconds{1}).action ==
                    dmxwb::ArtNetAction::dmx_committed,
        "missing sequence numbers do not block a newer packet");
}

void test_stale_sequence_does_not_refresh_liveness_and_restart_recovers() {
    auto core = *dmxwb::ArtNetCore::create({23});
    const auto t0 = dmxwb::ArtNetCore::time_point{};
    const std::array<std::uint8_t, 2> old_frame{{128, 128}};
    const std::array<std::uint8_t, 2> restarted_frame{{1, 1}};

    auto result = core.process_datagram(make_dmx(23, 1, 128, old_frame), kIpA, t0);
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed &&
                    core.last_artdmx_time() == std::optional{t0},
        "accepted high Sequence establishes source activity baseline");

    result = core.process_datagram(
        make_dmx(23, 1, 1, restarted_frame),
        kIpA,
        t0 + std::chrono::seconds{1});
    expect_true(result.action == dmxwb::ArtNetAction::ignored_stale_sequence &&
                    core.last_artdmx_time() == std::optional{t0},
        "restarted low Sequence is stale and does not refresh source activity");

    result = core.process_datagram(
        make_dmx(23, 2, 2, restarted_frame),
        kIpB,
        t0 + std::chrono::milliseconds{1500});
    expect_true(result.action == dmxwb::ArtNetAction::conflict &&
                    core.source_state() == dmxwb::ArtNetSourceState::conflict,
        "different source establishes conflict without changing accepted activity");

    result = core.process_datagram(
        make_dmx(23, 1, 1, restarted_frame),
        kIpA,
        t0 + std::chrono::milliseconds{2900});
    expect_true(result.action == dmxwb::ArtNetAction::ignored_stale_sequence &&
                    core.source_state() == dmxwb::ArtNetSourceState::conflict &&
                    core.last_artdmx_time() == std::optional{t0},
        "stale active-source traffic neither clears conflict nor postpones LOST");

    result = core.tick(t0 + dmxwb::kArtNetSourceLossTimeout);
    expect_true(result.action == dmxwb::ArtNetAction::source_lost &&
                    core.source_state() == dmxwb::ArtNetSourceState::lost &&
                    !core.active_source().has_value() && !core.last_sequence().has_value(),
        "source becomes LOST on accepted-traffic deadline despite rejected packets");
    expect_true(core.channel(1) == 128,
        "LOST retains the last accepted whole Art-Net snapshot");

    result = core.process_datagram(
        make_dmx(23, 1, 1, restarted_frame),
        kIpA,
        t0 + dmxwb::kArtNetSourceLossTimeout + std::chrono::milliseconds{1});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed &&
                    core.last_sequence() == std::optional<std::uint8_t>{1} &&
                    core.channel(1) == 1,
        "first low Sequence after LOST establishes restarted controller baseline");
}

void test_source_identity_conflict_and_loss_release() {
    auto core = *dmxwb::ArtNetCore::create({3});
    const auto t0 = dmxwb::ArtNetCore::time_point{};
    const std::array<std::uint8_t, 2> first{{1, 2}};
    const std::array<std::uint8_t, 2> other{{9, 9}};

    auto result = core.process_datagram(make_dmx(3, 7, 1, first), kIpA, t0);
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed &&
                core.source_state() == dmxwb::ArtNetSourceState::active,
        "first valid ArtDmx source becomes ACTIVE");
    expect_true(core.active_source() == std::optional<dmxwb::ArtNetSource>{{kIpA, 7}},
        "source lock identity contains IPv4 plus Physical");

    result = core.process_datagram(make_dmx(3, 7, 1, other), kIpB, t0 + std::chrono::milliseconds{1});
    expect_true(result.action == dmxwb::ArtNetAction::conflict &&
                core.source_state() == dmxwb::ArtNetSourceState::conflict,
        "different IPv4 on same Port-Address enters CONFLICT");
    expect_true(core.conflicting_source() ==
                    std::optional<dmxwb::ArtNetSource>{{kIpB, 7}},
        "conflict diagnostics identify the competing IPv4 and Physical source");
    expect_true(core.channel(1) == 1, "conflicting source data ignored with no merge");

    result = core.process_datagram(make_dmx(3, 8, 2, other), kIpA, t0 + std::chrono::milliseconds{2});
    expect_true(result.action == dmxwb::ArtNetAction::conflict,
        "same IPv4 with different Physical is also CONFLICT");
    expect_true(core.channel(1) == 1, "same-IP/different-Physical conflict cannot alter state");

    result = core.process_datagram(make_dmx(3, 7, 2, first), kIpA, t0 + std::chrono::milliseconds{3});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed &&
                core.source_state() == dmxwb::ArtNetSourceState::active,
        "current locked source remains accepted after conflict event");
    expect_true(!core.conflicting_source().has_value(),
        "accepted locked source clears the current conflict identity");

    result = core.tick(t0 + std::chrono::seconds{3} + std::chrono::milliseconds{3});
    expect_true(result.action == dmxwb::ArtNetAction::source_lost &&
                core.source_state() == dmxwb::ArtNetSourceState::lost && !core.active_source().has_value(),
        "three-second source timeout enters LOST and releases lock");
    expect_true(core.channel(1) == 1, "LOST keeps last committed Art-Net state");

    result = core.process_datagram(make_dmx(3, 4, 99, other), kIpB, t0 + std::chrono::seconds{3} + std::chrono::milliseconds{4});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed &&
                core.active_source() == std::optional<dmxwb::ArtNetSource>{{kIpB, 4}},
        "new source acquires released lock after LOST");
}

void test_art_sync_staging_release_source_filter_and_fallback() {
    auto core = *dmxwb::ArtNetCore::create({4});
    const auto t0 = dmxwb::ArtNetCore::time_point{};
    const std::array<std::uint8_t, 2> one{{1, 1}};
    const std::array<std::uint8_t, 2> two{{2, 2}};
    const std::array<std::uint8_t, 2> three{{3, 3}};
    const std::array<std::uint8_t, 2> four{{4, 4}};

    auto result = core.process_datagram(make_dmx(4, 1, 1, one), kIpA, t0);
    expect_true(result.action == dmxwb::ArtNetAction::dmx_committed, "initial asynchronous ArtDmx commits immediately");

    auto sync_with_aux = make_sync();
    sync_with_aux[12] = 0x55;
    sync_with_aux[13] = 0xaa;
    result = core.process_datagram(sync_with_aux, kIpA, t0 + std::chrono::milliseconds{1});
    expect_true(result.action == dmxwb::ArtNetAction::sync_entered &&
                core.sync_mode() == dmxwb::ArtNetSyncMode::synchronous,
        "matching ArtSync enters synchronous mode without testing Aux receiver fields");

    result = core.process_datagram(make_dmx(4, 1, 2, two), kIpA, t0 + std::chrono::milliseconds{2});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_staged && core.channel(1) == 1,
        "ArtDmx is staged without changing committed output in synchronous mode");

    result = core.process_datagram(make_sync(), kIpB, t0 + std::chrono::milliseconds{3});
    expect_true(result.action == dmxwb::ArtNetAction::ignored_sync_source && core.channel(1) == 1,
        "ArtSync from mismatched source IPv4 ignored");

    result = core.process_datagram(make_sync(), kIpA, t0 + std::chrono::milliseconds{4});
    expect_true(result.action == dmxwb::ArtNetAction::sync_committed && core.channel(1) == 2,
        "matching ArtSync atomically commits staged ArtDmx state");

    result = core.process_datagram(make_dmx(4, 1, 3, three), kIpA, t0 + std::chrono::seconds{2});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_staged && core.channel(1) == 2,
        "new synchronous ArtDmx remains staged");
    result = core.process_datagram(make_dmx(4, 1, 4, three), kIpA, t0 + std::chrono::seconds{4});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_staged,
        "continuing ArtDmx keeps active source lock while waiting for ArtSync");
    result = core.tick(t0 + std::chrono::seconds{4} + std::chrono::milliseconds{5});
    expect_true(result.action == dmxwb::ArtNetAction::sync_timeout_committed &&
                core.sync_mode() == dmxwb::ArtNetSyncMode::asynchronous && core.channel(1) == 3,
        "four-second ArtSync timeout returns async and commits latest staged data");

    result = core.process_datagram(make_sync(), kIpA, t0 + std::chrono::seconds{5});
    expect_true(result.action == dmxwb::ArtNetAction::sync_entered, "ArtSync can re-enter synchronous mode");
    result = core.process_datagram(make_dmx(4, 1, 5, four), kIpA, t0 + std::chrono::seconds{5} + std::chrono::milliseconds{1});
    expect_true(result.action == dmxwb::ArtNetAction::dmx_staged, "new value staged before source loss");
    result = core.tick(t0 + std::chrono::seconds{8} + std::chrono::milliseconds{1});
    expect_true(result.action == dmxwb::ArtNetAction::source_lost && core.channel(1) == 3,
        "source LOST discards unreleased synchronous staging and holds last committed state");
    expect_true(core.sync_mode() == dmxwb::ArtNetSyncMode::asynchronous,
        "source LOST resets synchronous mode");

    result = core.process_datagram(make_sync(13), kIpA, t0 + std::chrono::seconds{9});
    expect_true(result.rejected() && result.error == dmxwb::ArtNetParseError::protocol_revision_too_old,
        "ArtSync protocol revision below 14 rejected");
}

void test_targeted_art_poll() {
    auto core = *dmxwb::ArtNetCore::create({0x1234});
    const auto now = dmxwb::ArtNetCore::time_point{};

    auto poll_with_unused_flags = make_poll(false);
    poll_with_unused_flags[12] = static_cast<std::uint8_t>(poll_with_unused_flags[12] | 0xc0U);
    auto result = core.process_datagram(poll_with_unused_flags, kIpA, now);
    expect_true(result.action == dmxwb::ArtNetAction::poll_reply_requested,
        "non-targeted minimum-length ArtPoll requests reply and unused flag bits are not tested");

    result = core.process_datagram(make_poll(true, 0x1200, 0x1234), kIpA, now);
    expect_true(result.action == dmxwb::ArtNetAction::poll_reply_requested,
        "Targeted ArtPoll upper boundary is inclusive");

    result = core.process_datagram(make_poll(true, 0x1234, 0x1300), kIpA, now);
    expect_true(result.action == dmxwb::ArtNetAction::poll_reply_requested,
        "Targeted ArtPoll lower boundary is inclusive");

    result = core.process_datagram(make_poll(true, 0x0001, 0x1200), kIpA, now);
    expect_true(result.action == dmxwb::ArtNetAction::ignored_targeted_poll,
        "Targeted ArtPoll outside configured Port-Address ignored");

    result = core.process_datagram(make_poll(true, 0x1300, 0x1200), kIpA, now);
    expect_true(result.action == dmxwb::ArtNetAction::ignored_targeted_poll,
        "invalid reversed Targeted ArtPoll range does not request reply");

    auto zero_core = *dmxwb::ArtNetCore::create({0});
    result = zero_core.process_datagram(make_poll(true), kIpA, now);
    expect_true(result.action == dmxwb::ArtNetAction::poll_reply_requested,
        "14-byte Targeted ArtPoll assumes missing target fields are zero");

    result = core.process_datagram(make_poll(false, std::nullopt, std::nullopt, 13), kIpA, now);
    expect_true(result.rejected() && result.error == dmxwb::ArtNetParseError::protocol_revision_too_old,
        "ArtPoll protocol revision below 14 rejected");
}

void test_art_poll_reply_fields_and_explicit_oem_identity() {
    dmxwb::ArtNetPollReplyIdentity identity;
    identity.ip = {{192, 168, 10, 50}};
    identity.mac = {{0x02, 0x11, 0x22, 0x33, 0x44, 0x55}};
    identity.firmware_version = 0x0102;
    identity.poll_reply_counter = 42;
    identity.port_name = "DMXWB Port";
    identity.long_name = "DMXWB Art-Net 4 output";

    expect_true(!dmxwb::build_art_poll_reply(0x1234, identity).has_value(),
        "ArtPollReply cannot be built without explicit registered OEM identity");

    // Test-only sentinel verifies byte order. It is deliberately not a production OEM assignment.
    identity.oem_code = 0xbeef;
    auto reply = dmxwb::build_art_poll_reply(0x1234, identity);
    expect_true(reply.has_value(), "ArtPollReply builds after explicit OEM code is supplied");
    if (!reply.has_value()) return;

    const auto& bytes = reply->bytes;
    expect_true(bytes.size() == dmxwb::kArtNetPollReplyPacketSize,
        "ArtPollReply uses current full 239-byte packet layout");
    expect_true(bytes[0] == 'A' && bytes[7] == 0 && bytes[8] == 0x00 && bytes[9] == 0x21,
        "ArtPollReply ID and little-endian OpPollReply encoded");
    expect_true(bytes[10] == 192 && bytes[11] == 168 && bytes[12] == 10 && bytes[13] == 50,
        "ArtPollReply IPv4 encoded most-significant octet first");
    expect_true(bytes[14] == 0x36 && bytes[15] == 0x19,
        "ArtPollReply UDP Port 0x1936 encoded little-endian");
    expect_true(bytes[16] == 0x01 && bytes[17] == 0x02,
        "ArtPollReply firmware version encoded high byte first");
    expect_true(bytes[18] == 0x12 && bytes[19] == 0x03 && bytes[190] == 0x04,
        "ArtPollReply NetSwitch/SubSwitch/SwOut advertise full 15-bit Port-Address");
    expect_true(bytes[20] == 0xbe && bytes[21] == 0xef,
        "ArtPollReply explicit OEM code encoded high byte first");
    const std::string_view node_report{reinterpret_cast<const char*>(bytes.data() + 108), 18};
    expect_true(node_report == "#0001 [0042] DMXWB",
        "ArtPollReply NodeReport uses valid status/counter format");
    expect_true(bytes[172] == 0 && bytes[173] == 1 && bytes[174] == 0x80,
        "ArtPollReply advertises exactly one DMX512 output port");
    expect_true(bytes[182] == 0x00 && bytes[190] == 0x04,
        "inactive Art-Net source keeps GoodOutput data bit clear while subscription remains advertised");
    expect_true(bytes[212] == 0x08,
        "ArtPollReply Status2 declares 15-bit Port-Address support");
    expect_true(bytes[213] == 0xc0,
        "ArtPollReply GoodOutputB declares RDM disabled and continuous output style");
    expect_true(bytes[217] == 0x00,
        "ArtPollReply Status3 advertises Hold Last failsafe");
    expect_true(bytes[226] == 0 && bytes[227] == 44,
        "ArtPollReply RefreshRate explicitly advertises 44 Hz");

    identity.artnet_output_active = true;
    reply = dmxwb::build_art_poll_reply(0x1234, identity);
    expect_true(reply.has_value() && reply->bytes[182] == 0x80,
        "GoodOutputA bit7 set only when ArtDmx is actually selected/output physically");

    expect_true(!dmxwb::build_art_poll_reply(32768, identity).has_value(),
        "ArtPollReply rejects Port-Address outside 15-bit range");
}

}  // namespace

int main() {
    test_core_configuration_and_header_validation();
    test_art_dmx_validation_trailing_and_port_address();
    test_persistent_512_state_and_300_projection();
    test_sequence_rollover_stale_and_zero_disable();
    test_stale_sequence_does_not_refresh_liveness_and_restart_recovers();
    test_source_identity_conflict_and_loss_release();
    test_art_sync_staging_release_source_filter_and_fallback();
    test_targeted_art_poll();
    test_art_poll_reply_fields_and_explicit_oem_identity();

    if (failures != 0) {
        std::cerr << failures << " DEV-009 Art-Net core test(s) failed\n";
        return 1;
    }
    std::cout << "DMXWB DEV-009 Art-Net protocol core tests PASS\n";
    return 0;
}
