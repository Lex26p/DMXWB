#include "dmxwb/persistence.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dmxwb {
namespace {

enum class JsonKind {
    null_value,
    boolean,
    number,
    string,
    array,
    object,
};

struct JsonValue final {
    JsonKind kind{JsonKind::null_value};
    bool boolean{false};
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::string> object_keys;
    std::vector<JsonValue> object_values;
};

class JsonParser final {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    [[nodiscard]] bool parse(JsonValue& value) {
        skip_whitespace();
        if (!parse_value(value, 0)) {
            return false;
        }
        skip_whitespace();
        if (position_ != input_.size()) {
            set_error("unexpected trailing characters");
            return false;
        }
        return true;
    }

    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

private:
    static constexpr std::size_t kMaxDepth = 64;

    [[nodiscard]] bool parse_value(JsonValue& value, std::size_t depth) {
        if (depth > kMaxDepth) {
            set_error("JSON nesting is too deep");
            return false;
        }
        if (position_ >= input_.size()) {
            set_error("unexpected end of JSON");
            return false;
        }

        const char current = input_[position_];
        if (current == '{') {
            return parse_object(value, depth + 1);
        }
        if (current == '[') {
            return parse_array(value, depth + 1);
        }
        if (current == '"') {
            value.kind = JsonKind::string;
            return parse_string(value.text);
        }
        if (current == 't') {
            if (!consume_literal("true")) return false;
            value.kind = JsonKind::boolean;
            value.boolean = true;
            return true;
        }
        if (current == 'f') {
            if (!consume_literal("false")) return false;
            value.kind = JsonKind::boolean;
            value.boolean = false;
            return true;
        }
        if (current == 'n') {
            if (!consume_literal("null")) return false;
            value.kind = JsonKind::null_value;
            return true;
        }
        if (current == '-' || (current >= '0' && current <= '9')) {
            value.kind = JsonKind::number;
            return parse_number(value.text);
        }

        set_error("unexpected JSON token");
        return false;
    }

    [[nodiscard]] bool parse_object(JsonValue& value, std::size_t depth) {
        ++position_;
        value.kind = JsonKind::object;
        value.object_keys.clear();
        value.object_values.clear();
        skip_whitespace();
        if (consume_if('}')) {
            return true;
        }

        while (true) {
            if (position_ >= input_.size() || input_[position_] != '"') {
                set_error("object key must be a string");
                return false;
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            for (const auto& existing_key : value.object_keys) {
                if (existing_key == key) {
                    set_error("duplicate object key");
                    return false;
                }
            }
            skip_whitespace();
            if (!consume_if(':')) {
                set_error("expected ':' after object key");
                return false;
            }
            skip_whitespace();
            JsonValue child;
            if (!parse_value(child, depth)) {
                return false;
            }
            value.object_keys.push_back(std::move(key));
            value.object_values.push_back(std::move(child));
            skip_whitespace();
            if (consume_if('}')) {
                return true;
            }
            if (!consume_if(',')) {
                set_error("expected ',' or '}' in object");
                return false;
            }
            skip_whitespace();
        }
    }

    [[nodiscard]] bool parse_array(JsonValue& value, std::size_t depth) {
        ++position_;
        value.kind = JsonKind::array;
        value.array.clear();
        skip_whitespace();
        if (consume_if(']')) {
            return true;
        }

        while (true) {
            JsonValue child;
            if (!parse_value(child, depth)) {
                return false;
            }
            value.array.push_back(std::move(child));
            skip_whitespace();
            if (consume_if(']')) {
                return true;
            }
            if (!consume_if(',')) {
                set_error("expected ',' or ']' in array");
                return false;
            }
            skip_whitespace();
        }
    }

    [[nodiscard]] bool parse_string(std::string& output) {
        if (!consume_if('"')) {
            set_error("expected string");
            return false;
        }
        output.clear();

        while (position_ < input_.size()) {
            const auto byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == static_cast<unsigned char>('"')) {
                return true;
            }
            if (byte < 0x20U) {
                set_error("unescaped control character in string");
                return false;
            }
            if (byte != static_cast<unsigned char>('\\')) {
                output.push_back(static_cast<char>(byte));
                continue;
            }

            if (position_ >= input_.size()) {
                set_error("incomplete string escape");
                return false;
            }
            const char escape = input_[position_++];
            switch (escape) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = 0;
                    if (!parse_hex4(codepoint)) {
                        return false;
                    }
                    if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                        if (position_ + 2 > input_.size() || input_[position_] != '\\' || input_[position_ + 1] != 'u') {
                            set_error("high surrogate without low surrogate");
                            return false;
                        }
                        position_ += 2;
                        std::uint32_t low = 0;
                        if (!parse_hex4(low)) {
                            return false;
                        }
                        if (low < 0xDC00U || low > 0xDFFFU) {
                            set_error("invalid low surrogate");
                            return false;
                        }
                        codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (low - 0xDC00U);
                    } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                        set_error("unexpected low surrogate");
                        return false;
                    }
                    append_utf8(codepoint, output);
                    break;
                }
                default:
                    set_error("invalid string escape");
                    return false;
            }
        }

        set_error("unterminated string");
        return false;
    }

    [[nodiscard]] bool parse_hex4(std::uint32_t& value) {
        if (position_ + 4 > input_.size()) {
            set_error("incomplete unicode escape");
            return false;
        }
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const char ch = input_[position_++];
            std::uint32_t nibble = 0;
            if (ch >= '0' && ch <= '9') {
                nibble = static_cast<std::uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                nibble = static_cast<std::uint32_t>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                nibble = static_cast<std::uint32_t>(ch - 'A' + 10);
            } else {
                set_error("invalid unicode escape");
                return false;
            }
            value = (value << 4U) | nibble;
        }
        return true;
    }

    static void append_utf8(std::uint32_t codepoint, std::string& output) {
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

    [[nodiscard]] bool parse_number(std::string& token) {
        const auto start = position_;
        if (consume_if('-') && position_ >= input_.size()) {
            set_error("incomplete number");
            return false;
        }

        if (position_ >= input_.size()) {
            set_error("incomplete number");
            return false;
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
                set_error("leading zero in number");
                return false;
            }
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
                ++position_;
            }
        } else {
            set_error("invalid number");
            return false;
        }

        if (consume_if('.')) {
            const auto fraction_start = position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
                ++position_;
            }
            if (position_ == fraction_start) {
                set_error("fraction requires digits");
                return false;
            }
        }

        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const auto exponent_start = position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
                ++position_;
            }
            if (position_ == exponent_start) {
                set_error("exponent requires digits");
                return false;
            }
        }

        token.assign(input_.substr(start, position_ - start));
        return true;
    }

    [[nodiscard]] bool consume_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            set_error("invalid literal");
            return false;
        }
        position_ += literal.size();
        return true;
    }

    [[nodiscard]] bool consume_if(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skip_whitespace() noexcept {
        while (position_ < input_.size()) {
            const char ch = input_[position_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                break;
            }
            ++position_;
        }
    }

    void set_error(std::string_view message) {
        if (error_.empty()) {
            error_ = std::string{message} + " at byte " + std::to_string(position_);
        }
    }

    std::string_view input_;
    std::size_t position_{0};
    std::string error_;
};

[[nodiscard]] PersistenceError make_error(PersistenceErrorCode code, std::string message) {
    return PersistenceError{code, std::move(message)};
}

[[nodiscard]] const JsonValue* object_field(const JsonValue& object, std::string_view name) {
    if (object.kind != JsonKind::object) {
        return nullptr;
    }
    for (std::size_t index = 0; index < object.object_keys.size(); ++index) {
        if (object.object_keys[index] == name) {
            return &object.object_values[index];
        }
    }
    return nullptr;
}

[[nodiscard]] bool has_exact_fields(
    const JsonValue& object,
    std::initializer_list<std::string_view> expected) {
    if (object.kind != JsonKind::object || object.object_keys.size() != expected.size() ||
        object.object_values.size() != object.object_keys.size()) {
        return false;
    }
    for (const auto& key : object.object_keys) {
        bool found = false;
        for (const auto expected_key : expected) {
            if (key == expected_key) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool json_u64(const JsonValue& value, std::uint64_t& output) {
    if (value.kind != JsonKind::number || value.text.empty() || value.text.front() == '-' ||
        value.text.find_first_of(".eE") != std::string::npos) {
        return false;
    }
    const char* begin = value.text.data();
    const char* end = begin + value.text.size();
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    output = parsed;
    return true;
}

[[nodiscard]] bool json_size(const JsonValue& value, std::size_t& output) {
    std::uint64_t parsed = 0;
    if (!json_u64(value, parsed) || parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    output = static_cast<std::size_t>(parsed);
    return true;
}

[[nodiscard]] bool json_u32(const JsonValue& value, std::uint32_t& output) {
    std::uint64_t parsed = 0;
    if (!json_u64(value, parsed) || parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    output = static_cast<std::uint32_t>(parsed);
    return true;
}

[[nodiscard]] bool json_u16(const JsonValue& value, std::uint16_t& output) {
    std::uint64_t parsed = 0;
    if (!json_u64(value, parsed) || parsed > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    output = static_cast<std::uint16_t>(parsed);
    return true;
}

[[nodiscard]] bool json_u8(const JsonValue& value, std::uint8_t& output) {
    std::uint64_t parsed = 0;
    if (!json_u64(value, parsed) || parsed > std::numeric_limits<std::uint8_t>::max()) {
        return false;
    }
    output = static_cast<std::uint8_t>(parsed);
    return true;
}

[[nodiscard]] bool json_bool(const JsonValue& value, bool& output) {
    if (value.kind != JsonKind::boolean) {
        return false;
    }
    output = value.boolean;
    return true;
}

[[nodiscard]] bool json_string(const JsonValue& value, std::string& output) {
    if (value.kind != JsonKind::string) {
        return false;
    }
    output = value.text;
    return true;
}

void append_json_string(std::string& output, std::string_view value) {
    output.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte_value : value) {
        const auto byte = static_cast<unsigned char>(byte_value);
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

[[nodiscard]] bool supported_dmx_port(std::string_view port) noexcept {
    return port == "/dev/ttyRS485-1" || port == "/dev/ttyRS485-2";
}

[[nodiscard]] PersistenceError parse_fixture_config_record(
    const JsonValue& value,
    FixtureConfigRecord& record) {
    if (!has_exact_fields(value, {"id", "name"})) {
        return make_error(PersistenceErrorCode::schema, "fixture config record schema mismatch");
    }
    const auto* id = object_field(value, "id");
    const auto* name = object_field(value, "name");
    if (id == nullptr || name == nullptr || !json_u64(*id, record.id) || !json_string(*name, record.name)) {
        return make_error(PersistenceErrorCode::schema, "fixture config record has invalid field type");
    }
    return {};
}

[[nodiscard]] PersistenceError parse_group_config_record(
    const JsonValue& value,
    GroupConfigRecord& record) {
    if (!has_exact_fields(value, {"id", "name", "members"})) {
        return make_error(PersistenceErrorCode::schema, "group config record schema mismatch");
    }
    const auto* id = object_field(value, "id");
    const auto* name = object_field(value, "name");
    const auto* members = object_field(value, "members");
    if (id == nullptr || name == nullptr || members == nullptr ||
        !json_u64(*id, record.id) || !json_string(*name, record.name) || members->kind != JsonKind::array) {
        return make_error(PersistenceErrorCode::schema, "group config record has invalid field type");
    }
    record.members.clear();
    record.members.reserve(members->array.size());
    for (const auto& member : members->array) {
        Fixture::Id member_id = 0;
        if (!json_u64(member, member_id)) {
            return make_error(PersistenceErrorCode::schema, "group member ID must be an integer");
        }
        record.members.push_back(member_id);
    }
    return {};
}

[[nodiscard]] PersistenceError parse_scene_fixture_record(
    const JsonValue& value,
    SceneFixtureRecord& record) {
    if (!has_exact_fields(
            value,
            {"fixture_id", "red", "green", "blue", "white", "brightness", "requested_power"})) {
        return make_error(PersistenceErrorCode::schema, "scene fixture record schema mismatch");
    }
    const auto* fixture_id = object_field(value, "fixture_id");
    const auto* red = object_field(value, "red");
    const auto* green = object_field(value, "green");
    const auto* blue = object_field(value, "blue");
    const auto* white = object_field(value, "white");
    const auto* brightness = object_field(value, "brightness");
    const auto* requested_power = object_field(value, "requested_power");
    if (fixture_id == nullptr || red == nullptr || green == nullptr || blue == nullptr || white == nullptr ||
        brightness == nullptr || requested_power == nullptr ||
        !json_u64(*fixture_id, record.fixture_id) || !json_u8(*red, record.rgbw.red) ||
        !json_u8(*green, record.rgbw.green) || !json_u8(*blue, record.rgbw.blue) ||
        !json_u8(*white, record.rgbw.white) || !json_u8(*brightness, record.brightness) ||
        !json_bool(*requested_power, record.requested_power)) {
        return make_error(PersistenceErrorCode::schema, "scene fixture record has invalid field type");
    }
    return {};
}

[[nodiscard]] PersistenceError parse_scene_config_record(
    const JsonValue& value,
    SceneConfigRecord& record) {
    if (!has_exact_fields(value, {"id", "name", "fixtures"})) {
        return make_error(PersistenceErrorCode::schema, "scene config record schema mismatch");
    }
    const auto* id = object_field(value, "id");
    const auto* name = object_field(value, "name");
    const auto* fixtures = object_field(value, "fixtures");
    if (id == nullptr || name == nullptr || fixtures == nullptr ||
        !json_u64(*id, record.id) || !json_string(*name, record.name) || fixtures->kind != JsonKind::array) {
        return make_error(PersistenceErrorCode::schema, "scene config record has invalid field type");
    }
    record.fixtures.clear();
    record.fixtures.reserve(fixtures->array.size());
    for (const auto& fixture_value : fixtures->array) {
        SceneFixtureRecord fixture;
        const auto error = parse_scene_fixture_record(fixture_value, fixture);
        if (error) {
            return error;
        }
        record.fixtures.push_back(std::move(fixture));
    }
    return {};
}

[[nodiscard]] PersistenceError parse_runtime_fixture_record(
    const JsonValue& value,
    FixtureRuntimeState& record) {
    if (!has_exact_fields(
            value,
            {"id", "requested_power", "red", "green", "blue", "white", "brightness", "temperature"})) {
        return make_error(PersistenceErrorCode::schema, "runtime fixture record schema mismatch");
    }
    const auto* id = object_field(value, "id");
    const auto* requested_power = object_field(value, "requested_power");
    const auto* red = object_field(value, "red");
    const auto* green = object_field(value, "green");
    const auto* blue = object_field(value, "blue");
    const auto* white = object_field(value, "white");
    const auto* brightness = object_field(value, "brightness");
    const auto* temperature = object_field(value, "temperature");
    if (id == nullptr || requested_power == nullptr || red == nullptr || green == nullptr || blue == nullptr ||
        white == nullptr || brightness == nullptr || temperature == nullptr ||
        !json_u64(*id, record.id) || !json_bool(*requested_power, record.requested_power) ||
        !json_u8(*red, record.rgbw.red) || !json_u8(*green, record.rgbw.green) ||
        !json_u8(*blue, record.rgbw.blue) || !json_u8(*white, record.rgbw.white) ||
        !json_u8(*brightness, record.brightness) || !json_u8(*temperature, record.temperature)) {
        return make_error(PersistenceErrorCode::schema, "runtime fixture record has invalid field type");
    }
    return {};
}

}  // namespace

AppConfig make_default_config() {
    return AppConfig{};
}

AppState make_default_state(const AppConfig& config) {
    AppState state;
    state.fixtures.reserve(config.fixtures.size());
    for (const auto& fixture : config.fixtures) {
        FixtureRuntimeState runtime;
        runtime.id = fixture.id;
        state.fixtures.push_back(runtime);
    }
    return state;
}

bool is_valid_utf8(std::string_view text) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = bytes[index];
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t codepoint = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1;
            codepoint = static_cast<std::uint32_t>(first & 0x1FU);
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2;
            codepoint = static_cast<std::uint32_t>(first & 0x0FU);
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3;
            codepoint = static_cast<std::uint32_t>(first & 0x07U);
        } else {
            return false;
        }

        if (continuation_count > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = bytes[index + offset];
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | static_cast<std::uint32_t>(next & 0x3FU);
        }

        if ((continuation_count == 2U && codepoint < 0x800U) ||
            (continuation_count == 3U && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
            codepoint > 0x10FFFFU) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

PersistenceError validate_config(const AppConfig& config) {
    if (config.version != kPersistenceVersion) {
        return make_error(PersistenceErrorCode::version, "unsupported config version");
    }
    if (!supported_dmx_port(config.dmx_port)) {
        return make_error(PersistenceErrorCode::validation, "unsupported DMX port");
    }
    if (config.artnet_universe > kArtNetUniverseMax) {
        return make_error(PersistenceErrorCode::validation, "Art-Net universe is outside 0..32767");
    }
    if (config.fixture_count != config.fixtures.size()) {
        return make_error(PersistenceErrorCode::validation, "fixture count does not match fixture records");
    }
    if (config.fixture_count == 0) {
        if (config.start_address < 1 || config.start_address > kDmxPhysicalMaxSlots) {
            return make_error(PersistenceErrorCode::validation, "empty fixture configuration has invalid start address");
        }
    } else if (!calculate_slot_count(config.start_address, config.fixture_count, kFixtureChannels).has_value()) {
        return make_error(PersistenceErrorCode::validation, "fixture addressing exceeds physical slot 300");
    }

    std::unordered_set<Fixture::Id> fixture_ids;
    fixture_ids.reserve(config.fixtures.size());
    Fixture::Id max_fixture_id = 0;
    for (const auto& fixture : config.fixtures) {
        if (fixture.id == 0 || !fixture_ids.insert(fixture.id).second) {
            return make_error(PersistenceErrorCode::validation, "fixture IDs must be non-zero and unique");
        }
        if (fixture.name.size() > kEntityNameMaxBytes || !is_valid_utf8(fixture.name)) {
            return make_error(PersistenceErrorCode::validation, "fixture name must be valid UTF-8 up to 256 bytes");
        }
        max_fixture_id = std::max(max_fixture_id, fixture.id);
    }
    if (config.id_counters.next_fixture_id == 0 || config.id_counters.next_fixture_id <= max_fixture_id) {
        return make_error(PersistenceErrorCode::validation, "next_fixture_id must be greater than every fixture ID");
    }

    std::unordered_set<GroupId> group_ids;
    group_ids.reserve(config.groups.size());
    GroupId max_group_id = 0;
    for (const auto& group : config.groups) {
        if (group.id == 0 || !group_ids.insert(group.id).second) {
            return make_error(PersistenceErrorCode::validation, "group IDs must be non-zero and unique");
        }
        if (group.name.size() > kEntityNameMaxBytes || !is_valid_utf8(group.name)) {
            return make_error(PersistenceErrorCode::validation, "group name must be valid UTF-8 up to 256 bytes");
        }
        max_group_id = std::max(max_group_id, group.id);
        std::unordered_set<Fixture::Id> members;
        members.reserve(group.members.size());
        for (const auto member : group.members) {
            if (fixture_ids.find(member) == fixture_ids.end()) {
                return make_error(PersistenceErrorCode::validation, "group references missing fixture ID");
            }
            if (!members.insert(member).second) {
                return make_error(PersistenceErrorCode::validation, "group contains duplicate fixture member");
            }
        }
    }
    if (config.id_counters.next_group_id == 0 || config.id_counters.next_group_id <= max_group_id) {
        return make_error(PersistenceErrorCode::validation, "next_group_id must be greater than every group ID");
    }

    std::unordered_set<SceneId> scene_ids;
    scene_ids.reserve(config.scenes.size());
    SceneId max_scene_id = 0;
    for (const auto& scene : config.scenes) {
        if (scene.id == 0 || !scene_ids.insert(scene.id).second) {
            return make_error(PersistenceErrorCode::validation, "scene IDs must be non-zero and unique");
        }
        if (scene.name.size() > kEntityNameMaxBytes || !is_valid_utf8(scene.name)) {
            return make_error(PersistenceErrorCode::validation, "scene name must be valid UTF-8 up to 256 bytes");
        }
        max_scene_id = std::max(max_scene_id, scene.id);
        std::unordered_set<Fixture::Id> scene_fixture_ids;
        scene_fixture_ids.reserve(scene.fixtures.size());
        for (const auto& fixture : scene.fixtures) {
            // Scene snapshots may keep a deleted Fixture stable ID. Apply ignores
            // such records and IDs are never reused. Future/unallocated IDs are
            // still invalid: every historical ID is below next_fixture_id.
            if (fixture.fixture_id == 0 || fixture.fixture_id >= config.id_counters.next_fixture_id) {
                return make_error(PersistenceErrorCode::validation, "scene fixture ID was never allocated");
            }
            if (!scene_fixture_ids.insert(fixture.fixture_id).second) {
                return make_error(PersistenceErrorCode::validation, "scene contains duplicate fixture snapshot");
            }
            if (fixture.brightness > 100) {
                return make_error(PersistenceErrorCode::validation, "scene brightness is outside 0..100");
            }
        }
    }
    if (config.id_counters.next_scene_id == 0 || config.id_counters.next_scene_id <= max_scene_id) {
        return make_error(PersistenceErrorCode::validation, "next_scene_id must be greater than every scene ID");
    }

    return {};
}

PersistenceError validate_config_transition(
    const AppConfig& current_config,
    const AppConfig& proposed_config) {
    const auto proposed_error = validate_config(proposed_config);
    if (proposed_error) {
        return proposed_error;
    }

    if (proposed_config.id_counters.next_fixture_id < current_config.id_counters.next_fixture_id ||
        proposed_config.id_counters.next_group_id < current_config.id_counters.next_group_id ||
        proposed_config.id_counters.next_scene_id < current_config.id_counters.next_scene_id) {
        return make_error(PersistenceErrorCode::validation, "stable ID counters cannot move backwards");
    }

    std::unordered_set<Fixture::Id> current_fixture_ids;
    current_fixture_ids.reserve(current_config.fixtures.size());
    for (const auto& fixture : current_config.fixtures) {
        current_fixture_ids.insert(fixture.id);
    }
    for (const auto& fixture : proposed_config.fixtures) {
        if (!current_fixture_ids.contains(fixture.id) &&
            fixture.id < current_config.id_counters.next_fixture_id) {
            return make_error(PersistenceErrorCode::validation, "fixture stable ID was already allocated");
        }
    }

    std::unordered_set<GroupId> current_group_ids;
    current_group_ids.reserve(current_config.groups.size());
    for (const auto& group : current_config.groups) {
        current_group_ids.insert(group.id);
    }
    for (const auto& group : proposed_config.groups) {
        if (!current_group_ids.contains(group.id) &&
            group.id < current_config.id_counters.next_group_id) {
            return make_error(PersistenceErrorCode::validation, "group stable ID was already allocated");
        }
    }

    std::unordered_set<SceneId> current_scene_ids;
    current_scene_ids.reserve(current_config.scenes.size());
    for (const auto& scene : current_config.scenes) {
        current_scene_ids.insert(scene.id);
    }
    for (const auto& scene : proposed_config.scenes) {
        if (!current_scene_ids.contains(scene.id) &&
            scene.id < current_config.id_counters.next_scene_id) {
            return make_error(PersistenceErrorCode::validation, "scene stable ID was already allocated");
        }
    }

    return {};
}

PersistenceError validate_state(const AppState& state, const AppConfig& config) {
    if (state.version != kPersistenceVersion) {
        return make_error(PersistenceErrorCode::version, "unsupported state version");
    }
    if (state.source != PersistedSource::mqtt && state.source != PersistedSource::artnet) {
        return make_error(PersistenceErrorCode::validation, "state source is invalid");
    }
    const auto config_error = validate_config(config);
    if (config_error) {
        return config_error;
    }
    if (state.fixtures.size() != config.fixtures.size()) {
        return make_error(PersistenceErrorCode::validation, "state fixture set does not match config fixture set");
    }

    std::unordered_set<Fixture::Id> config_ids;
    config_ids.reserve(config.fixtures.size());
    for (const auto& fixture : config.fixtures) {
        config_ids.insert(fixture.id);
    }

    std::unordered_set<Fixture::Id> state_ids;
    state_ids.reserve(state.fixtures.size());
    for (const auto& fixture : state.fixtures) {
        if (fixture.id == 0 || !state_ids.insert(fixture.id).second || config_ids.find(fixture.id) == config_ids.end()) {
            return make_error(PersistenceErrorCode::validation, "state fixture IDs must exactly match config fixture IDs");
        }
        if (fixture.brightness > 100 || fixture.temperature > 100) {
            return make_error(PersistenceErrorCode::validation, "runtime brightness/temperature is outside 0..100");
        }
    }

    const auto validate_cleanup_ids = [](
        const std::vector<std::uint64_t>& ids,
        const std::unordered_set<std::uint64_t>& active_ids,
        std::uint64_t next_id,
        std::string_view entity) -> PersistenceError {
        std::unordered_set<std::uint64_t> unique;
        unique.reserve(ids.size());
        for (const auto id : ids) {
            if (id == 0 || id >= next_id) {
                return make_error(
                    PersistenceErrorCode::validation,
                    std::string{"pending MQTT cleanup references an unallocated "} +
                        std::string{entity} + " ID");
            }
            if (!unique.insert(id).second) {
                return make_error(
                    PersistenceErrorCode::validation,
                    std::string{"pending MQTT cleanup contains duplicate "} +
                        std::string{entity} + " ID");
            }
            if (active_ids.contains(id)) {
                return make_error(
                    PersistenceErrorCode::validation,
                    std::string{"pending MQTT cleanup references an active "} +
                        std::string{entity} + " ID");
            }
        }
        return {};
    };

    std::unordered_set<std::uint64_t> active_fixture_ids;
    active_fixture_ids.reserve(config.fixtures.size());
    for (const auto& fixture : config.fixtures) active_fixture_ids.insert(fixture.id);
    auto cleanup_error = validate_cleanup_ids(
        state.mqtt_retained_cleanup.fixture_ids,
        active_fixture_ids,
        config.id_counters.next_fixture_id,
        "Fixture");
    if (cleanup_error) return cleanup_error;

    std::unordered_set<std::uint64_t> active_group_ids;
    active_group_ids.reserve(config.groups.size());
    for (const auto& group : config.groups) active_group_ids.insert(group.id);
    cleanup_error = validate_cleanup_ids(
        state.mqtt_retained_cleanup.group_ids,
        active_group_ids,
        config.id_counters.next_group_id,
        "Group");
    if (cleanup_error) return cleanup_error;

    std::unordered_set<std::uint64_t> active_scene_ids;
    active_scene_ids.reserve(config.scenes.size());
    for (const auto& scene : config.scenes) active_scene_ids.insert(scene.id);
    cleanup_error = validate_cleanup_ids(
        state.mqtt_retained_cleanup.scene_ids,
        active_scene_ids,
        config.id_counters.next_scene_id,
        "Scene");
    if (cleanup_error) return cleanup_error;

    if (state.scene_create_idempotency.size() > kSceneCreateIdempotencyCapacity) {
        return make_error(
            PersistenceErrorCode::validation,
            "Scene Create idempotency history exceeds its bounded capacity");
    }
    std::unordered_set<std::string> request_ids;
    request_ids.reserve(state.scene_create_idempotency.size());
    for (const auto& record : state.scene_create_idempotency) {
        if (record.request_id.empty() ||
            record.request_id.size() > kEntityNameMaxBytes ||
            !is_valid_utf8(record.request_id) ||
            !request_ids.insert(record.request_id).second) {
            return make_error(
                PersistenceErrorCode::validation,
                "Scene Create idempotency request IDs must be unique valid UTF-8 up to 256 bytes");
        }
        if (record.name.size() > kEntityNameMaxBytes || !is_valid_utf8(record.name)) {
            return make_error(
                PersistenceErrorCode::validation,
                "Scene Create idempotency name must be valid UTF-8 up to 256 bytes");
        }
        if (record.scene_id == 0 ||
            record.scene_id >= config.id_counters.next_scene_id) {
            return make_error(
                PersistenceErrorCode::validation,
                "Scene Create idempotency references an unallocated Scene ID");
        }
        if (record.revision == 0 || record.revision > config.revision) {
            return make_error(
                PersistenceErrorCode::validation,
                "Scene Create idempotency revision is outside committed history");
        }
    }
    return {};
}

PersistenceResult<AppState> reconcile_state_for_config(
    const AppState& previous_state,
    const AppConfig& config) {
    const auto config_error = validate_config(config);
    if (config_error) {
        return {{}, config_error};
    }
    if (previous_state.version != kPersistenceVersion) {
        return {{}, make_error(PersistenceErrorCode::version, "unsupported state version")};
    }
    if (previous_state.source != PersistedSource::mqtt &&
        previous_state.source != PersistedSource::artnet) {
        return {{}, make_error(PersistenceErrorCode::validation, "state source is invalid")};
    }

    std::unordered_map<Fixture::Id, const FixtureRuntimeState*> previous_by_id;
    previous_by_id.reserve(previous_state.fixtures.size());
    for (const auto& fixture : previous_state.fixtures) {
        if (fixture.id == 0 ||
            !previous_by_id.emplace(fixture.id, &fixture).second) {
            return {{}, make_error(
                PersistenceErrorCode::validation,
                "state fixture IDs must be non-zero and unique")};
        }
        if (fixture.brightness > 100 || fixture.temperature > 100) {
            return {{}, make_error(
                PersistenceErrorCode::validation,
                "runtime brightness/temperature is outside 0..100")};
        }
    }

    AppState reconciled = make_default_state(config);
    reconciled.source = previous_state.source;
    reconciled.mqtt_retained_cleanup = previous_state.mqtt_retained_cleanup;
    reconciled.scene_create_idempotency = previous_state.scene_create_idempotency;
    for (auto& fixture : reconciled.fixtures) {
        const auto found = previous_by_id.find(fixture.id);
        if (found != previous_by_id.end()) {
            fixture = *found->second;
        }
    }

    const auto reconciled_error = validate_state(reconciled, config);
    if (reconciled_error) {
        return {{}, reconciled_error};
    }
    return {std::move(reconciled), {}};
}

PersistenceError validate_expected_revision(
    std::uint64_t current_revision,
    std::uint64_t expected_revision) {
    if (current_revision != expected_revision) {
        return make_error(PersistenceErrorCode::revision_conflict, "expected_revision does not match current revision");
    }
    return {};
}

std::string_view persisted_source_name(PersistedSource source) noexcept {
    switch (source) {
        case PersistedSource::mqtt: return "mqtt";
        case PersistedSource::artnet: return "artnet";
    }
    return "mqtt";
}

std::string serialize_config_json(const AppConfig& config) {
    std::string output;
    output.reserve(1024 + config.fixtures.size() * 64 + config.groups.size() * 96 + config.scenes.size() * 160);
    output += "{\n  \"version\": ";
    output += std::to_string(config.version);
    output += ",\n  \"revision\": ";
    output += std::to_string(config.revision);
    output += ",\n  \"dmx\": {\"port\": ";
    append_json_string(output, config.dmx_port);
    output += "},\n  \"artnet\": {\"universe\": ";
    output += std::to_string(config.artnet_universe);
    output += "},\n  \"fixtures\": {\n    \"count\": ";
    output += std::to_string(config.fixture_count);
    output += ",\n    \"start_address\": ";
    output += std::to_string(config.start_address);
    output += ",\n    \"items\": [";
    for (std::size_t index = 0; index < config.fixtures.size(); ++index) {
        const auto& fixture = config.fixtures[index];
        output += index == 0 ? "\n      {\"id\": " : ",\n      {\"id\": ";
        output += std::to_string(fixture.id);
        output += ", \"name\": ";
        append_json_string(output, fixture.name);
        output += "}";
    }
    if (!config.fixtures.empty()) output += "\n    ";
    output += "]\n  },\n  \"groups\": [";
    for (std::size_t index = 0; index < config.groups.size(); ++index) {
        const auto& group = config.groups[index];
        output += index == 0 ? "\n    {\"id\": " : ",\n    {\"id\": ";
        output += std::to_string(group.id);
        output += ", \"name\": ";
        append_json_string(output, group.name);
        output += ", \"members\": [";
        for (std::size_t member_index = 0; member_index < group.members.size(); ++member_index) {
            if (member_index != 0) output += ", ";
            output += std::to_string(group.members[member_index]);
        }
        output += "]}";
    }
    if (!config.groups.empty()) output += "\n  ";
    output += "],\n  \"scenes\": [";
    for (std::size_t index = 0; index < config.scenes.size(); ++index) {
        const auto& scene = config.scenes[index];
        output += index == 0 ? "\n    {\"id\": " : ",\n    {\"id\": ";
        output += std::to_string(scene.id);
        output += ", \"name\": ";
        append_json_string(output, scene.name);
        output += ", \"fixtures\": [";
        for (std::size_t fixture_index = 0; fixture_index < scene.fixtures.size(); ++fixture_index) {
            const auto& fixture = scene.fixtures[fixture_index];
            if (fixture_index != 0) output += ", ";
            output += "{\"fixture_id\": " + std::to_string(fixture.fixture_id);
            output += ", \"red\": " + std::to_string(fixture.rgbw.red);
            output += ", \"green\": " + std::to_string(fixture.rgbw.green);
            output += ", \"blue\": " + std::to_string(fixture.rgbw.blue);
            output += ", \"white\": " + std::to_string(fixture.rgbw.white);
            output += ", \"brightness\": " + std::to_string(fixture.brightness);
            output += ", \"requested_power\": ";
            output += fixture.requested_power ? "true" : "false";
            output += "}";
        }
        output += "]}";
    }
    if (!config.scenes.empty()) output += "\n  ";
    output += "],\n  \"id_counters\": {\n    \"next_fixture_id\": ";
    output += std::to_string(config.id_counters.next_fixture_id);
    output += ",\n    \"next_group_id\": ";
    output += std::to_string(config.id_counters.next_group_id);
    output += ",\n    \"next_scene_id\": ";
    output += std::to_string(config.id_counters.next_scene_id);
    output += "\n  }\n}\n";
    return output;
}

std::string serialize_state_json(const AppState& state) {
    std::string output;
    output.reserve(256 + state.fixtures.size() * 160);
    output += "{\n  \"version\": ";
    output += std::to_string(state.version);
    output += ",\n  \"source\": ";
    append_json_string(output, persisted_source_name(state.source));
    output += ",\n  \"fixtures\": [";
    for (std::size_t index = 0; index < state.fixtures.size(); ++index) {
        const auto& fixture = state.fixtures[index];
        output += index == 0 ? "\n    {\"id\": " : ",\n    {\"id\": ";
        output += std::to_string(fixture.id);
        output += ", \"requested_power\": ";
        output += fixture.requested_power ? "true" : "false";
        output += ", \"red\": " + std::to_string(fixture.rgbw.red);
        output += ", \"green\": " + std::to_string(fixture.rgbw.green);
        output += ", \"blue\": " + std::to_string(fixture.rgbw.blue);
        output += ", \"white\": " + std::to_string(fixture.rgbw.white);
        output += ", \"brightness\": " + std::to_string(fixture.brightness);
        output += ", \"temperature\": " + std::to_string(fixture.temperature);
        output += "}";
    }
    if (!state.fixtures.empty()) output += "\n  ";
    output += "],\n  \"mqtt_retained_cleanup\": {\n    \"fixture_ids\": [";
    for (std::size_t index = 0; index < state.mqtt_retained_cleanup.fixture_ids.size(); ++index) {
        if (index != 0) output += ", ";
        output += std::to_string(state.mqtt_retained_cleanup.fixture_ids[index]);
    }
    output += "],\n    \"group_ids\": [";
    for (std::size_t index = 0; index < state.mqtt_retained_cleanup.group_ids.size(); ++index) {
        if (index != 0) output += ", ";
        output += std::to_string(state.mqtt_retained_cleanup.group_ids[index]);
    }
    output += "],\n    \"scene_ids\": [";
    for (std::size_t index = 0; index < state.mqtt_retained_cleanup.scene_ids.size(); ++index) {
        if (index != 0) output += ", ";
        output += std::to_string(state.mqtt_retained_cleanup.scene_ids[index]);
    }
    output += "]\n  },\n  \"scene_create_idempotency\": [";
    for (std::size_t index = 0; index < state.scene_create_idempotency.size(); ++index) {
        const auto& record = state.scene_create_idempotency[index];
        output += index == 0 ? "\n    {\"request_id\": " : ",\n    {\"request_id\": ";
        append_json_string(output, record.request_id);
        output += ", \"name\": ";
        append_json_string(output, record.name);
        output += ", \"scene_id\": " + std::to_string(record.scene_id);
        output += ", \"revision\": " + std::to_string(record.revision) + "}";
    }
    if (!state.scene_create_idempotency.empty()) output += "\n  ";
    output += "]\n}\n";
    return output;
}

PersistenceResult<AppConfig> parse_config_json_unvalidated(std::string_view json) {
    JsonValue root;
    JsonParser parser{json};
    if (!parser.parse(root)) {
        return {{}, make_error(PersistenceErrorCode::json_syntax, parser.error())};
    }
    if (!has_exact_fields(root, {"version", "revision", "dmx", "artnet", "fixtures", "groups", "scenes", "id_counters"})) {
        return {{}, make_error(PersistenceErrorCode::schema, "config root schema mismatch")};
    }

    AppConfig config;
    const auto* version = object_field(root, "version");
    const auto* revision = object_field(root, "revision");
    const auto* dmx = object_field(root, "dmx");
    const auto* artnet = object_field(root, "artnet");
    const auto* fixtures = object_field(root, "fixtures");
    const auto* groups = object_field(root, "groups");
    const auto* scenes = object_field(root, "scenes");
    const auto* counters = object_field(root, "id_counters");
    if (version == nullptr || revision == nullptr || dmx == nullptr || artnet == nullptr || fixtures == nullptr ||
        groups == nullptr || scenes == nullptr || counters == nullptr ||
        !json_u32(*version, config.version) || !json_u64(*revision, config.revision)) {
        return {{}, make_error(PersistenceErrorCode::schema, "config version/revision has invalid type")};
    }
    if (config.version != kPersistenceVersion) {
        return {{}, make_error(PersistenceErrorCode::version, "unsupported config version")};
    }

    if (!has_exact_fields(*dmx, {"port"})) {
        return {{}, make_error(PersistenceErrorCode::schema, "dmx schema mismatch")};
    }
    const auto* port = object_field(*dmx, "port");
    if (port == nullptr || !json_string(*port, config.dmx_port)) {
        return {{}, make_error(PersistenceErrorCode::schema, "dmx.port must be a string")};
    }

    if (!has_exact_fields(*artnet, {"universe"})) {
        return {{}, make_error(PersistenceErrorCode::schema, "artnet schema mismatch")};
    }
    const auto* universe = object_field(*artnet, "universe");
    if (universe == nullptr || !json_u16(*universe, config.artnet_universe)) {
        return {{}, make_error(PersistenceErrorCode::schema, "artnet.universe must be an integer")};
    }

    if (!has_exact_fields(*fixtures, {"count", "start_address", "items"})) {
        return {{}, make_error(PersistenceErrorCode::schema, "fixtures schema mismatch")};
    }
    const auto* count = object_field(*fixtures, "count");
    const auto* start_address = object_field(*fixtures, "start_address");
    const auto* items = object_field(*fixtures, "items");
    if (count == nullptr || start_address == nullptr || items == nullptr ||
        !json_size(*count, config.fixture_count) || !json_size(*start_address, config.start_address) ||
        items->kind != JsonKind::array) {
        return {{}, make_error(PersistenceErrorCode::schema, "fixtures fields have invalid type")};
    }
    config.fixtures.clear();
    config.fixtures.reserve(items->array.size());
    for (const auto& item : items->array) {
        FixtureConfigRecord record;
        const auto error = parse_fixture_config_record(item, record);
        if (error) return {{}, error};
        config.fixtures.push_back(std::move(record));
    }

    if (groups->kind != JsonKind::array || scenes->kind != JsonKind::array) {
        return {{}, make_error(PersistenceErrorCode::schema, "groups/scenes must be arrays")};
    }
    config.groups.clear();
    config.groups.reserve(groups->array.size());
    for (const auto& item : groups->array) {
        GroupConfigRecord record;
        const auto error = parse_group_config_record(item, record);
        if (error) return {{}, error};
        config.groups.push_back(std::move(record));
    }
    config.scenes.clear();
    config.scenes.reserve(scenes->array.size());
    for (const auto& item : scenes->array) {
        SceneConfigRecord record;
        const auto error = parse_scene_config_record(item, record);
        if (error) return {{}, error};
        config.scenes.push_back(std::move(record));
    }

    if (!has_exact_fields(*counters, {"next_fixture_id", "next_group_id", "next_scene_id"})) {
        return {{}, make_error(PersistenceErrorCode::schema, "id_counters schema mismatch")};
    }
    const auto* next_fixture_id = object_field(*counters, "next_fixture_id");
    const auto* next_group_id = object_field(*counters, "next_group_id");
    const auto* next_scene_id = object_field(*counters, "next_scene_id");
    if (next_fixture_id == nullptr || next_group_id == nullptr || next_scene_id == nullptr ||
        !json_u64(*next_fixture_id, config.id_counters.next_fixture_id) ||
        !json_u64(*next_group_id, config.id_counters.next_group_id) ||
        !json_u64(*next_scene_id, config.id_counters.next_scene_id)) {
        return {{}, make_error(PersistenceErrorCode::schema, "id counters must be integers")};
    }

    return {std::move(config), {}};
}

PersistenceResult<AppConfig> parse_config_json(std::string_view json) {
    auto parsed = parse_config_json_unvalidated(json);
    if (!parsed.ok()) {
        return parsed;
    }
    const auto validation_error = validate_config(*parsed.value);
    if (validation_error) {
        return {{}, validation_error};
    }
    return parsed;
}

PersistenceResult<AppState> parse_state_json(std::string_view json) {
    JsonValue root;
    JsonParser parser{json};
    if (!parser.parse(root)) {
        return {{}, make_error(PersistenceErrorCode::json_syntax, parser.error())};
    }
    const bool legacy_schema = has_exact_fields(root, {"version", "source", "fixtures"});
    const bool cleanup_schema = has_exact_fields(
        root,
        {"version", "source", "fixtures", "mqtt_retained_cleanup"});
    const bool current_schema = has_exact_fields(
        root,
        {"version", "source", "fixtures", "mqtt_retained_cleanup", "scene_create_idempotency"});
    if (!legacy_schema && !cleanup_schema && !current_schema) {
        return {{}, make_error(PersistenceErrorCode::schema, "state root schema mismatch")};
    }

    AppState state;
    const auto* version = object_field(root, "version");
    const auto* source = object_field(root, "source");
    const auto* fixtures = object_field(root, "fixtures");
    std::string source_text;
    if (version == nullptr || source == nullptr || fixtures == nullptr ||
        !json_u32(*version, state.version) || !json_string(*source, source_text) || fixtures->kind != JsonKind::array) {
        return {{}, make_error(PersistenceErrorCode::schema, "state fields have invalid type")};
    }
    if (state.version != kPersistenceVersion) {
        return {{}, make_error(PersistenceErrorCode::version, "unsupported state version")};
    }
    if (source_text == "mqtt") {
        state.source = PersistedSource::mqtt;
    } else if (source_text == "artnet") {
        state.source = PersistedSource::artnet;
    } else {
        return {{}, make_error(PersistenceErrorCode::validation, "state source must be mqtt or artnet")};
    }

    state.fixtures.clear();
    state.fixtures.reserve(fixtures->array.size());
    for (const auto& item : fixtures->array) {
        FixtureRuntimeState record;
        const auto error = parse_runtime_fixture_record(item, record);
        if (error) return {{}, error};
        if (record.brightness > 100 || record.temperature > 100) {
            return {{}, make_error(PersistenceErrorCode::validation, "runtime brightness/temperature is outside 0..100")};
        }
        state.fixtures.push_back(std::move(record));
    }

    if (cleanup_schema || current_schema) {
        const auto* cleanup = object_field(root, "mqtt_retained_cleanup");
        if (cleanup == nullptr ||
            !has_exact_fields(*cleanup, {"fixture_ids", "group_ids", "scene_ids"})) {
            return {{}, make_error(
                PersistenceErrorCode::schema,
                "mqtt_retained_cleanup schema mismatch")};
        }

        const auto parse_ids = [](const JsonValue* value, auto& output) -> PersistenceError {
            if (value == nullptr || value->kind != JsonKind::array) {
                return make_error(
                    PersistenceErrorCode::schema,
                    "mqtt_retained_cleanup IDs must be arrays");
            }
            output.clear();
            output.reserve(value->array.size());
            for (const auto& item : value->array) {
                std::uint64_t id = 0;
                if (!json_u64(item, id)) {
                    return make_error(
                        PersistenceErrorCode::schema,
                        "mqtt_retained_cleanup IDs must be integers");
                }
                output.push_back(id);
            }
            return {};
        };

        auto error = parse_ids(
            object_field(*cleanup, "fixture_ids"),
            state.mqtt_retained_cleanup.fixture_ids);
        if (error) return {{}, std::move(error)};
        error = parse_ids(
            object_field(*cleanup, "group_ids"),
            state.mqtt_retained_cleanup.group_ids);
        if (error) return {{}, std::move(error)};
        error = parse_ids(
            object_field(*cleanup, "scene_ids"),
            state.mqtt_retained_cleanup.scene_ids);
        if (error) return {{}, std::move(error)};
    }

    if (current_schema) {
        const auto* records = object_field(root, "scene_create_idempotency");
        if (records == nullptr || records->kind != JsonKind::array) {
            return {{}, make_error(
                PersistenceErrorCode::schema,
                "scene_create_idempotency must be an array")};
        }
        state.scene_create_idempotency.clear();
        state.scene_create_idempotency.reserve(records->array.size());
        for (const auto& value : records->array) {
            if (!has_exact_fields(value, {"request_id", "name", "scene_id", "revision"})) {
                return {{}, make_error(
                    PersistenceErrorCode::schema,
                    "scene_create_idempotency record schema mismatch")};
            }
            SceneCreateIdempotencyRecord record;
            const auto* request_id = object_field(value, "request_id");
            const auto* name = object_field(value, "name");
            const auto* scene_id = object_field(value, "scene_id");
            const auto* revision = object_field(value, "revision");
            if (request_id == nullptr || name == nullptr || scene_id == nullptr ||
                revision == nullptr || !json_string(*request_id, record.request_id) ||
                !json_string(*name, record.name) || !json_u64(*scene_id, record.scene_id) ||
                !json_u64(*revision, record.revision)) {
                return {{}, make_error(
                    PersistenceErrorCode::schema,
                    "scene_create_idempotency record has invalid field type")};
            }
            state.scene_create_idempotency.push_back(std::move(record));
        }
    }
    return {std::move(state), {}};
}

PersistenceError restore_fixture_collection(
    const AppConfig& config,
    const AppState& state,
    FixtureCollection& collection) {
    const auto state_error = validate_state(state, config);
    if (state_error) {
        return state_error;
    }

    std::unordered_map<Fixture::Id, const FixtureRuntimeState*> runtime_by_id;
    runtime_by_id.reserve(state.fixtures.size());
    for (const auto& runtime : state.fixtures) {
        runtime_by_id.emplace(runtime.id, &runtime);
    }

    std::vector<Fixture> fixtures;
    fixtures.reserve(config.fixtures.size());
    for (const auto& configured : config.fixtures) {
        const auto found = runtime_by_id.find(configured.id);
        if (found == runtime_by_id.end()) {
            return make_error(PersistenceErrorCode::validation, "state is missing configured fixture");
        }
        Fixture fixture{configured.id, configured.name};
        const auto* runtime = found->second;
        if (!fixture.restore_state(
                runtime->requested_power,
                runtime->rgbw,
                runtime->brightness,
                runtime->temperature)) {
            return make_error(PersistenceErrorCode::validation, "fixture runtime state cannot be restored");
        }
        fixtures.push_back(std::move(fixture));
    }

    if (!collection.restore(std::move(fixtures), config.start_address, config.id_counters.next_fixture_id)) {
        return make_error(PersistenceErrorCode::validation, "fixture collection rejected persisted IDs/addressing");
    }
    return {};
}

}  // namespace dmxwb
