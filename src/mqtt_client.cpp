#include "dmxwb/mqtt_client.hpp"

#include <mosquitto.h>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace dmxwb {
namespace {

constexpr int kMqttQos = 1;
constexpr int kMqttKeepaliveSeconds = 30;
constexpr unsigned int kReconnectDelaySeconds = 1;
constexpr unsigned int kReconnectDelayMaxSeconds = 30;
constexpr std::string_view kMqttClientId = "dmxwb";
constexpr std::string_view kSystemStatusTopic = "/devices/dmxwb/controls/status";
constexpr std::string_view kOfflinePayload = "off";

[[nodiscard]] bool payload_size_fits_int(std::size_t size) noexcept {
    return size <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

}  // namespace

MqttClient::MqttClient(MqttCommandQueue& command_queue)
    : command_queue_(command_queue) {}

MqttClient::~MqttClient() {
    stop();
}

bool MqttClient::start(std::string host, std::uint16_t port) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (host.empty() || port == 0) {
        set_error("MQTT broker host/port is invalid");
        return false;
    }

    if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
        set_error("mosquitto_lib_init failed");
        return false;
    }
    lib_initialized_ = true;

    host_ = std::move(host);
    port_ = port;
    mosq_ = mosquitto_new(kMqttClientId.data(), true, this);
    if (mosq_ == nullptr) {
        set_error("mosquitto_new failed");
        cleanup_after_start_failure();
        return false;
    }

    mosquitto_connect_callback_set(mosq_, &MqttClient::on_connect);
    mosquitto_disconnect_callback_set(mosq_, &MqttClient::on_disconnect);
    mosquitto_message_callback_set(mosq_, &MqttClient::on_message);

    const int will_result = mosquitto_will_set(
        mosq_,
        kSystemStatusTopic.data(),
        static_cast<int>(kOfflinePayload.size()),
        kOfflinePayload.data(),
        kMqttQos,
        true);
    if (will_result != MOSQ_ERR_SUCCESS) {
        set_error(std::string{"mosquitto_will_set: "} + mosquitto_strerror(will_result));
        cleanup_after_start_failure();
        return false;
    }

    const int reconnect_result = mosquitto_reconnect_delay_set(
        mosq_,
        kReconnectDelaySeconds,
        kReconnectDelayMaxSeconds,
        true);
    if (reconnect_result != MOSQ_ERR_SUCCESS) {
        set_error(std::string{"mosquitto_reconnect_delay_set: "} + mosquitto_strerror(reconnect_result));
        cleanup_after_start_failure();
        return false;
    }

    const int connect_result = mosquitto_connect_async(
        mosq_,
        host_.c_str(),
        static_cast<int>(port_),
        kMqttKeepaliveSeconds);
    if (connect_result != MOSQ_ERR_SUCCESS) {
        set_error(std::string{"mosquitto_connect_async: "} + mosquitto_strerror(connect_result));
        cleanup_after_start_failure();
        return false;
    }

    const int loop_result = mosquitto_loop_start(mosq_);
    if (loop_result != MOSQ_ERR_SUCCESS) {
        set_error(std::string{"mosquitto_loop_start: "} + mosquitto_strerror(loop_result));
        cleanup_after_start_failure();
        return false;
    }

    running_.store(true, std::memory_order_release);
    return true;
}

void MqttClient::stop() noexcept {
    if (mosq_ != nullptr) {
        const bool was_connected = connected_.load(std::memory_order_acquire);
        if (was_connected) {
            MqttPublication offline{
                std::string{kSystemStatusTopic},
                std::string{kOfflinePayload},
                true};
            (void)publish(offline);
            (void)mosquitto_disconnect(mosq_);
        }
        (void)mosquitto_loop_stop(mosq_, !was_connected);
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }

    connected_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    full_republish_requested_.store(false, std::memory_order_release);
    if (lib_initialized_) {
        (void)mosquitto_lib_cleanup();
        lib_initialized_ = false;
    }
}

bool MqttClient::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool MqttClient::connected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

bool MqttClient::publish(const MqttPublication& publication) {
    if (mosq_ == nullptr || !connected_.load(std::memory_order_acquire) ||
        publication.topic.empty() || !payload_size_fits_int(publication.payload.size())) {
        publish_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const int result = mosquitto_publish(
        mosq_,
        nullptr,
        publication.topic.c_str(),
        static_cast<int>(publication.payload.size()),
        publication.payload.data(),
        kMqttQos,
        publication.retained);
    if (result != MOSQ_ERR_SUCCESS) {
        publish_failures_.fetch_add(1, std::memory_order_relaxed);
        set_error(std::string{"mosquitto_publish: "} + mosquitto_strerror(result));
        return false;
    }
    return true;
}

bool MqttClient::publish_all(std::span<const MqttPublication> publications) {
    for (const auto& publication : publications) {
        if (!publish(publication)) {
            full_republish_requested_.store(true, std::memory_order_release);
            return false;
        }
    }
    return true;
}

bool MqttClient::take_full_republish_request() noexcept {
    return full_republish_requested_.exchange(false, std::memory_order_acq_rel);
}

MqttClientDiagnostics MqttClient::diagnostics() const {
    MqttClientDiagnostics result;
    result.running = running();
    result.connected = connected();
    result.successful_connections = successful_connections_.load(std::memory_order_relaxed);
    result.disconnects = disconnects_.load(std::memory_order_relaxed);
    result.commands_accepted = commands_accepted_.load(std::memory_order_relaxed);
    result.commands_ignored = commands_ignored_.load(std::memory_order_relaxed);
    result.commands_rejected = commands_rejected_.load(std::memory_order_relaxed);
    result.publish_failures = publish_failures_.load(std::memory_order_relaxed);
    result.callback_failures = callback_failures_.load(std::memory_order_relaxed);
    {
        std::lock_guard lock{error_mutex_};
        result.last_error = last_error_;
    }
    return result;
}

void MqttClient::on_connect(struct mosquitto* mosq, void* userdata, int rc) noexcept {
    auto* self = static_cast<MqttClient*>(userdata);
    if (self == nullptr) {
        return;
    }
    try {
        self->handle_connect(mosq, rc);
    } catch (...) {
        self->callback_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

void MqttClient::on_disconnect(struct mosquitto*, void* userdata, int rc) noexcept {
    auto* self = static_cast<MqttClient*>(userdata);
    if (self == nullptr) {
        return;
    }
    try {
        self->handle_disconnect(rc);
    } catch (...) {
        self->callback_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

void MqttClient::on_message(
    struct mosquitto*,
    void* userdata,
    const struct mosquitto_message* message) noexcept {
    auto* self = static_cast<MqttClient*>(userdata);
    if (self == nullptr) {
        return;
    }
    try {
        self->handle_message(message);
    } catch (...) {
        self->callback_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

void MqttClient::handle_connect(struct mosquitto* mosq, int rc) {
    if (rc != 0) {
        connected_.store(false, std::memory_order_release);
        set_error(std::string{"MQTT CONNACK: "} + mosquitto_connack_string(rc));
        return;
    }

    const int command_result = mosquitto_subscribe(
        mosq,
        nullptr,
        kMqttDeviceCommandSubscription.data(),
        kMqttQos);
    const int config_result = mosquitto_subscribe(
        mosq,
        nullptr,
        kMqttConfigSetTopic.data(),
        kMqttQos);
    if (command_result != MOSQ_ERR_SUCCESS || config_result != MOSQ_ERR_SUCCESS) {
        connected_.store(false, std::memory_order_release);
        if (command_result != MOSQ_ERR_SUCCESS) {
            set_error(std::string{"MQTT command subscribe: "} + mosquitto_strerror(command_result));
        } else {
            set_error(std::string{"MQTT config subscribe: "} + mosquitto_strerror(config_result));
        }
        (void)mosquitto_disconnect(mosq);
        return;
    }

    connected_.store(true, std::memory_order_release);
    successful_connections_.fetch_add(1, std::memory_order_relaxed);
    full_republish_requested_.store(true, std::memory_order_release);
}

void MqttClient::handle_disconnect(int rc) {
    connected_.store(false, std::memory_order_release);
    disconnects_.fetch_add(1, std::memory_order_relaxed);
    if (rc != 0) {
        set_error(std::string{"MQTT disconnected: "} + mosquitto_strerror(rc));
    }
}

void MqttClient::handle_message(const struct mosquitto_message* message) {
    if (message == nullptr || message->topic == nullptr || message->payloadlen < 0 ||
        (message->payloadlen > 0 && message->payload == nullptr)) {
        commands_rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto payload_size = static_cast<std::size_t>(message->payloadlen);
    const auto* payload_bytes = static_cast<const char*>(message->payload);
    const std::string_view payload = payload_size == 0
        ? std::string_view{}
        : std::string_view{payload_bytes, payload_size};
    const auto parsed = parse_mqtt_command(message->topic, payload, message->retain);
    switch (parsed.status) {
        case MqttCommandParseStatus::accepted:
            if (parsed.command.has_value()) {
                command_queue_.push(*parsed.command);
                commands_accepted_.fetch_add(1, std::memory_order_relaxed);
            } else {
                commands_rejected_.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case MqttCommandParseStatus::ignored:
            commands_ignored_.fetch_add(1, std::memory_order_relaxed);
            break;
        case MqttCommandParseStatus::rejected:
            commands_rejected_.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

void MqttClient::set_error(std::string message) {
    std::lock_guard lock{error_mutex_};
    last_error_ = std::move(message);
}

void MqttClient::cleanup_after_start_failure() noexcept {
    if (mosq_ != nullptr) {
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }
    if (lib_initialized_) {
        (void)mosquitto_lib_cleanup();
        lib_initialized_ = false;
    }
    running_.store(false, std::memory_order_release);
    connected_.store(false, std::memory_order_release);
}

}  // namespace dmxwb
