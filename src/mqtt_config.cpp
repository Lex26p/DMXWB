#include "dmxwb/mqtt_config.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace dmxwb {
namespace {

constexpr std::size_t kMaxEnvelopeDepth = 64;
constexpr std::size_t kMaxRequestIdBytes = 256;

struct JsonSlice final {
    std::size_t begin{0};
    std::size_t end{0};
    bool present{false};
};

void skip_whitespace(std::string_view input, std::size_t& position) noexcept {
    while (position < input.size()) {
        const char c = input[position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        ++position;
    }
}

[[nodiscard]] bool is_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] std::uint32_t hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c - '0');
    if (c >= 'a' && c <= 'f') return 10U + static_cast<std::uint32_t>(c - 'a');
    return 10U + static_cast<std::uint32_t>(c - 'A');
}

[[nodiscard]] bool scan_json_string(
    std::string_view input,
    std::size_t& position,
    std::string& error) {
    if (position >= input.size() || input[position] != '"') {
        error = "expected JSON string";
        return false;
    }
    ++position;
    while (position < input.size()) {
        const auto byte = static_cast<unsigned char>(input[position]);
        if (byte == '"') {
            ++position;
            return true;
        }
        if (byte < 0x20U) {
            error = "control character in JSON string";
            return false;
        }
        if (byte != '\\') {
            ++position;
            continue;
        }

        ++position;
        if (position >= input.size()) {
            error = "unterminated JSON escape";
            return false;
        }
        const char escaped = input[position++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                break;
            case 'u':
                if (input.size() - position < 4U) {
                    error = "short JSON unicode escape";
                    return false;
                }
                for (std::size_t index = 0; index < 4U; ++index) {
                    if (!is_hex(input[position + index])) {
                        error = "invalid JSON unicode escape";
                        return false;
                    }
                }
                position += 4U;
                break;
            default:
                error = "invalid JSON escape";
                return false;
        }
    }
    error = "unterminated JSON string";
    return false;
}

[[nodiscard]] bool scan_json_number(
    std::string_view input,
    std::size_t& position,
    std::string& error) {
    const std::size_t begin = position;
    if (position < input.size() && input[position] == '-') {
        ++position;
    }
    if (position >= input.size()) {
        error = "incomplete JSON number";
        return false;
    }
    if (input[position] == '0') {
        ++position;
        if (position < input.size() && input[position] >= '0' && input[position] <= '9') {
            error = "leading zero in JSON number";
            return false;
        }
    } else if (input[position] >= '1' && input[position] <= '9') {
        while (position < input.size() && input[position] >= '0' && input[position] <= '9') {
            ++position;
        }
    } else {
        error = "invalid JSON number";
        return false;
    }

    if (position < input.size() && input[position] == '.') {
        ++position;
        const std::size_t fraction_begin = position;
        while (position < input.size() && input[position] >= '0' && input[position] <= '9') {
            ++position;
        }
        if (position == fraction_begin) {
            error = "invalid JSON fraction";
            return false;
        }
    }

    if (position < input.size() && (input[position] == 'e' || input[position] == 'E')) {
        ++position;
        if (position < input.size() && (input[position] == '+' || input[position] == '-')) {
            ++position;
        }
        const std::size_t exponent_begin = position;
        while (position < input.size() && input[position] >= '0' && input[position] <= '9') {
            ++position;
        }
        if (position == exponent_begin) {
            error = "invalid JSON exponent";
            return false;
        }
    }
    return position > begin;
}

[[nodiscard]] bool scan_json_value(
    std::string_view input,
    std::size_t& position,
    std::size_t depth,
    std::string& error);

[[nodiscard]] bool scan_json_array(
    std::string_view input,
    std::size_t& position,
    std::size_t depth,
    std::string& error) {
    ++position;
    skip_whitespace(input, position);
    if (position < input.size() && input[position] == ']') {
        ++position;
        return true;
    }
    while (position < input.size()) {
        if (!scan_json_value(input, position, depth + 1U, error)) {
            return false;
        }
        skip_whitespace(input, position);
        if (position < input.size() && input[position] == ']') {
            ++position;
            return true;
        }
        if (position >= input.size() || input[position] != ',') {
            error = "expected ',' or ']' in JSON array";
            return false;
        }
        ++position;
        skip_whitespace(input, position);
    }
    error = "unterminated JSON array";
    return false;
}

[[nodiscard]] bool scan_json_object(
    std::string_view input,
    std::size_t& position,
    std::size_t depth,
    std::string& error) {
    ++position;
    skip_whitespace(input, position);
    if (position < input.size() && input[position] == '}') {
        ++position;
        return true;
    }
    while (position < input.size()) {
        if (!scan_json_string(input, position, error)) {
            return false;
        }
        skip_whitespace(input, position);
        if (position >= input.size() || input[position] != ':') {
            error = "expected ':' in JSON object";
            return false;
        }
        ++position;
        skip_whitespace(input, position);
        if (!scan_json_value(input, position, depth + 1U, error)) {
            return false;
        }
        skip_whitespace(input, position);
        if (position < input.size() && input[position] == '}') {
            ++position;
            return true;
        }
        if (position >= input.size() || input[position] != ',') {
            error = "expected ',' or '}' in JSON object";
            return false;
        }
        ++position;
        skip_whitespace(input, position);
    }
    error = "unterminated JSON object";
    return false;
}

[[nodiscard]] bool scan_json_value(
    std::string_view input,
    std::size_t& position,
    std::size_t depth,
    std::string& error) {
    if (depth > kMaxEnvelopeDepth) {
        error = "JSON nesting limit exceeded";
        return false;
    }
    skip_whitespace(input, position);
    if (position >= input.size()) {
        error = "missing JSON value";
        return false;
    }
    switch (input[position]) {
        case '"': return scan_json_string(input, position, error);
        case '{': return scan_json_object(input, position, depth, error);
        case '[': return scan_json_array(input, position, depth, error);
        case 't':
            if (input.substr(position, 4U) == "true") { position += 4U; return true; }
            break;
        case 'f':
            if (input.substr(position, 5U) == "false") { position += 5U; return true; }
            break;
        case 'n':
            if (input.substr(position, 4U) == "null") { position += 4U; return true; }
            break;
        default:
            if (input[position] == '-' || (input[position] >= '0' && input[position] <= '9')) {
                return scan_json_number(input, position, error);
            }
            break;
    }
    error = "invalid JSON value";
    return false;
}

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

[[nodiscard]] bool decode_json_string(
    std::string_view raw,
    std::string& output,
    std::string& error) {
    if (raw.size() < 2U || raw.front() != '"' || raw.back() != '"') {
        error = "request_id must be a JSON string";
        return false;
    }
    output.clear();
    for (std::size_t position = 1U; position + 1U < raw.size();) {
        const auto byte = static_cast<unsigned char>(raw[position++]);
        if (byte < 0x20U) {
            error = "request_id contains a control character";
            return false;
        }
        if (byte != '\\') {
            output.push_back(static_cast<char>(byte));
            continue;
        }
        if (position + 1U > raw.size()) {
            error = "invalid request_id escape";
            return false;
        }
        const char escaped = raw[position++];
        switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (position + 4U > raw.size() - 1U) {
                    error = "short request_id unicode escape";
                    return false;
                }
                std::uint32_t first = 0;
                for (std::size_t index = 0; index < 4U; ++index) {
                    if (!is_hex(raw[position + index])) {
                        error = "invalid request_id unicode escape";
                        return false;
                    }
                    first = (first << 4U) | hex_value(raw[position + index]);
                }
                position += 4U;
                std::uint32_t codepoint = first;
                if (first >= 0xD800U && first <= 0xDBFFU) {
                    if (position + 6U > raw.size() - 1U || raw[position] != '\\' || raw[position + 1U] != 'u') {
                        error = "unpaired high surrogate in request_id";
                        return false;
                    }
                    position += 2U;
                    std::uint32_t second = 0;
                    for (std::size_t index = 0; index < 4U; ++index) {
                        if (!is_hex(raw[position + index])) {
                            error = "invalid low surrogate in request_id";
                            return false;
                        }
                        second = (second << 4U) | hex_value(raw[position + index]);
                    }
                    position += 4U;
                    if (second < 0xDC00U || second > 0xDFFFU) {
                        error = "invalid low surrogate in request_id";
                        return false;
                    }
                    codepoint = 0x10000U + ((first - 0xD800U) << 10U) + (second - 0xDC00U);
                } else if (first >= 0xDC00U && first <= 0xDFFFU) {
                    error = "unpaired low surrogate in request_id";
                    return false;
                }
                append_utf8(output, codepoint);
                break;
            }
            default:
                error = "invalid request_id escape";
                return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = bytes[index];
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            count = 1;
            codepoint = static_cast<std::uint32_t>(first & 0x1FU);
        } else if (first >= 0xE0U && first <= 0xEFU) {
            count = 2;
            codepoint = static_cast<std::uint32_t>(first & 0x0FU);
        } else if (first >= 0xF0U && first <= 0xF4U) {
            count = 3;
            codepoint = static_cast<std::uint32_t>(first & 0x07U);
        } else {
            return false;
        }
        if (count > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1; offset <= count; ++offset) {
            const auto next = bytes[index + offset];
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | static_cast<std::uint32_t>(next & 0x3FU);
        }
        if ((count == 2U && codepoint < 0x800U) ||
            (count == 3U && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
            codepoint > 0x10FFFFU) {
            return false;
        }
        index += count + 1U;
    }
    return true;
}

[[nodiscard]] bool parse_key_at(
    std::string_view input,
    std::size_t& position,
    std::string& key,
    std::string& error) {
    const std::size_t begin = position;
    if (!scan_json_string(input, position, error)) {
        return false;
    }
    return decode_json_string(input.substr(begin, position - begin), key, error);
}

[[nodiscard]] bool parse_envelope_slices(
    std::string_view input,
    JsonSlice& request_id,
    JsonSlice& expected_revision,
    JsonSlice& config,
    std::string& error) {
    std::size_t position = 0;
    skip_whitespace(input, position);
    if (position >= input.size() || input[position] != '{') {
        error = "config/set root must be a JSON object";
        return false;
    }
    ++position;
    skip_whitespace(input, position);
    if (position < input.size() && input[position] == '}') {
        error = "config/set object is empty";
        return false;
    }

    while (position < input.size()) {
        std::string key;
        if (!parse_key_at(input, position, key, error)) {
            return false;
        }
        skip_whitespace(input, position);
        if (position >= input.size() || input[position] != ':') {
            error = "expected ':' after config/set field name";
            return false;
        }
        ++position;
        skip_whitespace(input, position);
        const std::size_t value_begin = position;
        if (!scan_json_value(input, position, 1U, error)) {
            return false;
        }
        const std::size_t value_end = position;

        JsonSlice* destination = nullptr;
        if (key == "request_id") {
            destination = &request_id;
        } else if (key == "expected_revision") {
            destination = &expected_revision;
        } else if (key == "config") {
            destination = &config;
        } else {
            error = "config/set contains an unknown field: " + key;
            return false;
        }
        if (destination->present) {
            error = "config/set contains a duplicate field: " + key;
            return false;
        }
        *destination = JsonSlice{value_begin, value_end, true};

        skip_whitespace(input, position);
        if (position < input.size() && input[position] == '}') {
            ++position;
            break;
        }
        if (position >= input.size() || input[position] != ',') {
            error = "expected ',' or '}' in config/set object";
            return false;
        }
        ++position;
        skip_whitespace(input, position);
    }

    skip_whitespace(input, position);
    if (position != input.size()) {
        error = "trailing data after config/set object";
        return false;
    }
    if (!request_id.present || !expected_revision.present || !config.present) {
        error = "config/set requires request_id, expected_revision and config";
        return false;
    }
    return true;
}

[[nodiscard]] bool parse_u64_json_integer(std::string_view raw, std::uint64_t& output) noexcept {
    if (raw.empty() || raw.front() == '-' || raw.find_first_of(".eE+") != std::string_view::npos) {
        return false;
    }
    if (raw.size() > 1U && raw.front() == '0') {
        return false;
    }
    const char* const begin = raw.data();
    const char* const end = begin + raw.size();
    const auto parsed = std::from_chars(begin, end, output);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

[[nodiscard]] std::string_view persistence_error_code_name(PersistenceErrorCode code) noexcept {
    switch (code) {
        case PersistenceErrorCode::none: return "none";
        case PersistenceErrorCode::json_syntax: return "json_syntax";
        case PersistenceErrorCode::schema: return "schema";
        case PersistenceErrorCode::version: return "version";
        case PersistenceErrorCode::validation: return "validation";
        case PersistenceErrorCode::revision_conflict: return "revision_conflict";
    }
    return "validation";
}

void append_json_string(std::string& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (byte < 0x20U) {
                    output += "\\u00";
                    output.push_back(hex[(byte >> 4U) & 0x0FU]);
                    output.push_back(hex[byte & 0x0FU]);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
                break;
        }
    }
    output.push_back('"');
}

}  // namespace

MqttConfigSetParseResult parse_mqtt_config_set_request(std::string_view payload) {
    MqttConfigSetParseResult result;
    JsonSlice request_id_slice;
    JsonSlice expected_revision_slice;
    JsonSlice config_slice;
    std::string envelope_error;
    if (!parse_envelope_slices(
            payload,
            request_id_slice,
            expected_revision_slice,
            config_slice,
            envelope_error)) {
        if (request_id_slice.present) {
            std::string ignored_error;
            (void)decode_json_string(
                payload.substr(request_id_slice.begin, request_id_slice.end - request_id_slice.begin),
                result.request_id,
                ignored_error);
            if (!is_valid_utf8(result.request_id)) {
                result.request_id.clear();
            }
        }
        result.error_code = "invalid_request";
        result.message = std::move(envelope_error);
        return result;
    }

    std::string request_id;
    if (!decode_json_string(
            payload.substr(request_id_slice.begin, request_id_slice.end - request_id_slice.begin),
            request_id,
            envelope_error) ||
        request_id.empty() || request_id.size() > kMaxRequestIdBytes || !is_valid_utf8(request_id)) {
        result.error_code = "invalid_request";
        result.message = envelope_error.empty()
            ? "request_id must be a non-empty valid UTF-8 string up to 256 bytes"
            : std::move(envelope_error);
        return result;
    }
    result.request_id = request_id;

    std::uint64_t expected_revision = 0;
    if (!parse_u64_json_integer(
            payload.substr(
                expected_revision_slice.begin,
                expected_revision_slice.end - expected_revision_slice.begin),
            expected_revision)) {
        result.error_code = "invalid_request";
        result.message = "expected_revision must be an unsigned 64-bit integer";
        return result;
    }

    const auto parsed_config = parse_config_json(
        payload.substr(config_slice.begin, config_slice.end - config_slice.begin));
    if (!parsed_config.ok()) {
        result.error_code = std::string{persistence_error_code_name(parsed_config.error.code)};
        result.message = parsed_config.error.message;
        return result;
    }
    if (serialize_config_json(*parsed_config.value).size() > kPersistenceMaxFileBytes) {
        result.error_code = "too_large";
        result.message = "canonical config exceeds persistence file size limit";
        return result;
    }
    if (parsed_config.value->revision != expected_revision) {
        result.error_code = "revision_conflict";
        result.message = "config.revision does not match expected_revision";
        return result;
    }

    result.request = MqttConfigSetRequest{
        std::move(request_id),
        expected_revision,
        std::move(*parsed_config.value)};
    return result;
}

std::string_view mqtt_config_file_error_code_name(PersistenceFileErrorCode code) noexcept {
    switch (code) {
        case PersistenceFileErrorCode::none: return "none";
        case PersistenceFileErrorCode::not_found: return "not_found";
        case PersistenceFileErrorCode::io: return "io";
        case PersistenceFileErrorCode::too_large: return "too_large";
        case PersistenceFileErrorCode::parse: return "parse";
        case PersistenceFileErrorCode::validation: return "validation";
        case PersistenceFileErrorCode::revision_conflict: return "revision_conflict";
    }
    return "io";
}

MqttPublication build_mqtt_config_result_publication(
    std::string_view request_id,
    bool ok,
    std::uint64_t revision,
    std::string_view error_code,
    std::string_view message) {
    std::string payload{"{\"request_id\":"};
    append_json_string(payload, request_id);
    payload += ",\"ok\":";
    payload += ok ? "true" : "false";
    payload += ",\"revision\":" + std::to_string(revision);
    payload += ",\"error_code\":";
    append_json_string(payload, error_code.empty() ? std::string_view{"none"} : error_code);
    payload += ",\"message\":";
    append_json_string(payload, message);
    payload += '}';
    return MqttPublication{std::string{kMqttConfigResultTopic}, std::move(payload), false};
}

}  // namespace dmxwb
