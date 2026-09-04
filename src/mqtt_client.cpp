#include "dmxwb/mqtt_client.hpp"

#include <mosquitto.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace dmxwb {
namespace {

constexpr int kMqttQos = 1;
constexpr int kMqttKeepaliveSeconds = 30;
constexpr unsigned int kReconnectDelaySeconds = 1;
constexpr unsigned int kReconnectDelayMaxSeconds = 30;
constexpr int kNetworkLoopTimeoutMilliseconds = 250;
constexpr std::string_view kMqttClientId = "dmxwb";
constexpr std::string_view kSystemStatusTopic = "/devices/dmxwb/controls/status";
constexpr std::string_view kOfflinePayload = "off";

[[nodiscard]] bool payload_size_fits_int(std::size_t size) noexcept {
    return size <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

}  // namespace

MqttClient::MqttClient(
    MqttCommandQueue& command_queue,
    InstrumentationMode instrumentation_mode)
    : command_queue_(command_queue),
      instrumentation_mode_(instrumentation_mode) {}

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
    mosquitto_publish_callback_set(mosq_, &MqttClient::on_publish);
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

    const int threaded_result = mosquitto_threaded_set(mosq_, true);
    if (threaded_result != MOSQ_ERR_SUCCESS) {
        set_error(std::string{"mosquitto_threaded_set: "} +
            mosquitto_strerror(threaded_result));
        cleanup_after_start_failure();
        return false;
    }

    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    try {
        network_thread_ = std::thread{&MqttClient::network_loop, this};
    } catch (const std::exception& error) {
        set_error(std::string{"Cannot start MQTT network worker: "} + error.what());
        running_.store(false, std::memory_order_release);
        cleanup_after_start_failure();
        return false;
    } catch (...) {
        set_error("Cannot start MQTT network worker");
        running_.store(false, std::memory_order_release);
        cleanup_after_start_failure();
        return false;
    }

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

            std::unique_lock lock{retry_mutex_};
            (void)retry_condition_.wait_for(
                lock,
                std::chrono::seconds{1},
                [this] {
                    return !connected_.load(std::memory_order_acquire);
                });
        }
    }

    stop_requested_.store(true, std::memory_order_release);
    retry_condition_.notify_all();

    if (network_thread_.joinable()) {
        network_thread_.join();
    }

    if (mosq_ != nullptr) {
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }

    connected_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    full_republish_requested_.store(false, std::memory_order_release);
    {
        std::lock_guard lock{cleanup_mutex_};
        cleanup_active_ = false;
        cleanup_message_ids_.clear();
        cleanup_delivery_ = MqttRetainedCleanupDelivery::none;
    }
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
        increment_engineering_counter(publish_failures_);
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
        increment_engineering_counter(publish_failures_);
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

bool MqttClient::publish_retained_cleanup(
    std::span<const MqttPublication> publications) {
    if (mosq_ == nullptr || !connected_.load(std::memory_order_acquire) ||
        publications.empty()) {
        return false;
    }

    std::lock_guard lock{cleanup_mutex_};
    if (cleanup_active_) {
        return false;
    }
    cleanup_delivery_ = MqttRetainedCleanupDelivery::none;
    cleanup_message_ids_.clear();
    cleanup_active_ = true;

    for (const auto& publication : publications) {
        if (publication.topic.empty() || !publication.retained ||
            !publication.payload.empty()) {
            cleanup_active_ = false;
            cleanup_message_ids_.clear();
            cleanup_delivery_ = MqttRetainedCleanupDelivery::failed;
            return false;
        }

        int message_id = 0;
        const int result = mosquitto_publish(
            mosq_,
            &message_id,
            publication.topic.c_str(),
            0,
            nullptr,
            kMqttQos,
            true);
        if (result != MOSQ_ERR_SUCCESS) {
            cleanup_active_ = false;
            cleanup_message_ids_.clear();
            cleanup_delivery_ = MqttRetainedCleanupDelivery::failed;
            increment_engineering_counter(publish_failures_);
            set_error(std::string{"mosquitto cleanup publish: "} +
                mosquitto_strerror(result));
            return false;
        }
        cleanup_message_ids_.insert(message_id);
    }
    return true;
}

MqttRetainedCleanupDelivery MqttClient::take_retained_cleanup_delivery() noexcept {
    std::lock_guard lock{cleanup_mutex_};
    const auto result = cleanup_delivery_;
    cleanup_delivery_ = MqttRetainedCleanupDelivery::none;
    return result;
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

MqttClientOperationalState MqttClient::operational_state() const {
    MqttClientOperationalState result;
    result.running = running();
    result.connected = connected();
    {
        std::lock_guard lock{error_mutex_};
        result.last_error = last_error_;
    }
    return result;
}

InstrumentationMode MqttClient::instrumentation_mode() const noexcept {
    return instrumentation_mode_;
}

void MqttClient::on_connect(struct mosquitto* mosq, void* userdata, int rc) noexcept {
    auto* self = static_cast<MqttClient*>(userdata);
    if (self == nullptr) {
        return;
    }
    try {
        self->handle_connect(mosq, rc);
    } catch (...) {
        self->increment_engineering_counter(self->callback_failures_);
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
        self->increment_engineering_counter(self->callback_failures_);
    }
}

void MqttClient::on_publish(
    struct mosquitto*,
    void* userdata,
    int message_id) noexcept {
    auto* self = static_cast<MqttClient*>(userdata);
    if (self == nullptr) {
        return;
    }
    self->handle_publish(message_id);
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
        self->increment_engineering_counter(self->callback_failures_);
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
    const int scene_lifecycle_result = mosquitto_subscribe(
        mosq,
        nullptr,
        kMqttSceneLifecycleSubscription.data(),
        kMqttQos);
    if (command_result != MOSQ_ERR_SUCCESS ||
        config_result != MOSQ_ERR_SUCCESS ||
        scene_lifecycle_result != MOSQ_ERR_SUCCESS) {
        connected_.store(false, std::memory_order_release);
        if (command_result != MOSQ_ERR_SUCCESS) {
            set_error(std::string{"MQTT command subscribe: "} + mosquitto_strerror(command_result));
        } else if (config_result != MOSQ_ERR_SUCCESS) {
            set_error(std::string{"MQTT config subscribe: "} + mosquitto_strerror(config_result));
        } else {
            set_error(std::string{"MQTT Scene lifecycle subscribe: "} +
                mosquitto_strerror(scene_lifecycle_result));
        }
        (void)mosquitto_disconnect(mosq);
        return;
    }

    connected_.store(true, std::memory_order_release);
    set_error({});
    increment_engineering_counter(successful_connections_);
    full_republish_requested_.store(true, std::memory_order_release);
}

void MqttClient::handle_disconnect(int rc) {
    connected_.store(false, std::memory_order_release);
    retry_condition_.notify_all();
    mark_cleanup_delivery_failed();
    increment_engineering_counter(disconnects_);
    if (rc != 0) {
        set_error(std::string{"MQTT disconnected: "} + mosquitto_strerror(rc));
    }
}

void MqttClient::network_loop() noexcept {
    try {
        unsigned int retry_delay_seconds = kReconnectDelaySeconds;
        bool first_attempt = true;

        while (!stop_requested_.load(std::memory_order_acquire)) {
            const int connect_result = first_attempt
                ? mosquitto_connect(
                      mosq_,
                      host_.c_str(),
                      static_cast<int>(port_),
                      kMqttKeepaliveSeconds)
                : mosquitto_reconnect(mosq_);
            first_attempt = false;

            if (connect_result != MOSQ_ERR_SUCCESS) {
                connected_.store(false, std::memory_order_release);
                set_error(std::string{"MQTT connect: "} +
                    mosquitto_strerror(connect_result));
                if (!wait_for_retry(retry_delay_seconds)) {
                    break;
                }
                retry_delay_seconds = std::min(
                    retry_delay_seconds * 2U,
                    kReconnectDelayMaxSeconds);
                continue;
            }

            bool connection_established = false;
            while (!stop_requested_.load(std::memory_order_acquire)) {
                const int loop_result = mosquitto_loop(
                    mosq_,
                    kNetworkLoopTimeoutMilliseconds,
                    1);
                if (connected_.load(std::memory_order_acquire)) {
                    connection_established = true;
                    retry_delay_seconds = kReconnectDelaySeconds;
                }
                if (loop_result == MOSQ_ERR_SUCCESS) {
                    continue;
                }
                if (stop_requested_.load(std::memory_order_acquire)) {
                    break;
                }

                connected_.store(false, std::memory_order_release);
                mark_cleanup_delivery_failed();
                set_error(std::string{"MQTT network loop: "} +
                    mosquitto_strerror(loop_result));
                break;
            }

            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            if (!wait_for_retry(retry_delay_seconds)) {
                break;
            }
            if (!connection_established) {
                retry_delay_seconds = std::min(
                    retry_delay_seconds * 2U,
                    kReconnectDelayMaxSeconds);
            }
        }
    } catch (const std::exception& error) {
        connected_.store(false, std::memory_order_release);
        try {
            set_error(std::string{"MQTT network worker failed: "} + error.what());
        } catch (...) {
        }
        running_.store(false, std::memory_order_release);
    } catch (...) {
        connected_.store(false, std::memory_order_release);
        try {
            set_error("MQTT network worker failed");
        } catch (...) {
        }
        running_.store(false, std::memory_order_release);
    }
}

bool MqttClient::wait_for_retry(unsigned int delay_seconds) {
    std::unique_lock lock{retry_mutex_};
    retry_condition_.wait_for(
        lock,
        std::chrono::seconds{delay_seconds},
        [this] {
            return stop_requested_.load(std::memory_order_acquire);
        });
    return !stop_requested_.load(std::memory_order_acquire);
}

void MqttClient::mark_cleanup_delivery_failed() noexcept {
    std::lock_guard lock{cleanup_mutex_};
    if (cleanup_active_) {
        cleanup_active_ = false;
        cleanup_message_ids_.clear();
        cleanup_delivery_ = MqttRetainedCleanupDelivery::failed;
    }
}

void MqttClient::handle_publish(int message_id) noexcept {
    std::lock_guard lock{cleanup_mutex_};
    if (!cleanup_active_ || cleanup_message_ids_.erase(message_id) == 0) {
        return;
    }
    if (cleanup_message_ids_.empty()) {
        cleanup_active_ = false;
        cleanup_delivery_ = MqttRetainedCleanupDelivery::delivered;
    }
}

void MqttClient::handle_message(const struct mosquitto_message* message) {
    if (message == nullptr || message->topic == nullptr || message->payloadlen < 0 ||
        (message->payloadlen > 0 && message->payload == nullptr)) {
        increment_engineering_counter(commands_rejected_);
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
                increment_engineering_counter(commands_accepted_);
            } else {
                increment_engineering_counter(commands_rejected_);
            }
            break;
        case MqttCommandParseStatus::ignored:
            increment_engineering_counter(commands_ignored_);
            break;
        case MqttCommandParseStatus::rejected:
            increment_engineering_counter(commands_rejected_);
            break;
    }
}

void MqttClient::set_error(std::string message) {
    std::lock_guard lock{error_mutex_};
    last_error_ = std::move(message);
}

void MqttClient::cleanup_after_start_failure() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    retry_condition_.notify_all();
    if (network_thread_.joinable()) {
        network_thread_.join();
    }
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
    std::lock_guard lock{cleanup_mutex_};
    cleanup_active_ = false;
    cleanup_message_ids_.clear();
    cleanup_delivery_ = MqttRetainedCleanupDelivery::none;
}

void MqttClient::increment_engineering_counter(
    std::atomic<std::uint64_t>& counter) noexcept {
    if (engineering_instrumentation_enabled(instrumentation_mode_)) {
        counter.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace dmxwb
