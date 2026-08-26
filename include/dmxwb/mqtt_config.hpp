#pragma once

#include "dmxwb/mqtt_contract.hpp"
#include "dmxwb/persistence_storage.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dmxwb {

struct MqttConfigSetRequest final {
    std::string request_id;
    std::uint64_t expected_revision{0};
    AppConfig proposed_config;
};

struct MqttConfigSetParseResult final {
    std::optional<MqttConfigSetRequest> request;
    std::string request_id;
    std::string error_code;
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return request.has_value() && error_code.empty();
    }
};

// Payload schema:
// {
//   "request_id": "opaque caller token",
//   "expected_revision": 7,
//   "config": { ...complete canonical AppConfig... }
// }
//
// Parsing runs in Controller context, not in the libmosquitto callback.
[[nodiscard]] MqttConfigSetParseResult parse_mqtt_config_set_request(std::string_view payload);

[[nodiscard]] std::string_view mqtt_config_file_error_code_name(
    PersistenceFileErrorCode code) noexcept;

[[nodiscard]] MqttPublication build_mqtt_config_result_publication(
    std::string_view request_id,
    bool ok,
    std::uint64_t revision,
    std::string_view error_code,
    std::string_view message);

}  // namespace dmxwb
