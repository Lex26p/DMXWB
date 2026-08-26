#pragma once

#include "dmxwb/mqtt_contract.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

struct mosquitto;
struct mosquitto_message;

namespace dmxwb {

struct MqttClientDiagnostics final {
    bool running{false};
    bool connected{false};
    std::uint64_t successful_connections{0};
    std::uint64_t disconnects{0};
    std::uint64_t commands_accepted{0};
    std::uint64_t commands_ignored{0};
    std::uint64_t commands_rejected{0};
    std::uint64_t publish_failures{0};
    std::uint64_t callback_failures{0};
    std::string last_error;
};

// Thin libmosquitto transport. Network callbacks only parse/enqueue Commands
// and set reconnect/resync flags. Fixture model, persistence and DmxOutput are
// deliberately owned by the Controller/main context, never by this callback.
class MqttClient final {
public:
    explicit MqttClient(MqttCommandQueue& command_queue);
    ~MqttClient();

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;
    MqttClient(MqttClient&&) = delete;
    MqttClient& operator=(MqttClient&&) = delete;

    [[nodiscard]] bool start(
        std::string host = std::string{kMqttBrokerHost},
        std::uint16_t port = kMqttBrokerPort);
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] bool publish(const MqttPublication& publication);
    [[nodiscard]] bool publish_all(std::span<const MqttPublication> publications);

    // on_connect sets this flag after subscriptions are recreated. The
    // Controller/main context consumes it and publishes the complete current
    // metadata/state from its own model.
    [[nodiscard]] bool take_full_republish_request() noexcept;

    [[nodiscard]] MqttClientDiagnostics diagnostics() const;

private:
    static void on_connect(struct mosquitto* mosq, void* userdata, int rc) noexcept;
    static void on_disconnect(struct mosquitto* mosq, void* userdata, int rc) noexcept;
    static void on_message(
        struct mosquitto* mosq,
        void* userdata,
        const struct mosquitto_message* message) noexcept;

    void handle_connect(struct mosquitto* mosq, int rc);
    void handle_disconnect(int rc);
    void handle_message(const struct mosquitto_message* message);
    void set_error(std::string message);
    void cleanup_after_start_failure() noexcept;

    MqttCommandQueue& command_queue_;
    struct mosquitto* mosq_{nullptr};
    std::string host_;
    std::uint16_t port_{kMqttBrokerPort};
    bool lib_initialized_{false};

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> full_republish_requested_{false};
    std::atomic<std::uint64_t> successful_connections_{0};
    std::atomic<std::uint64_t> disconnects_{0};
    std::atomic<std::uint64_t> commands_accepted_{0};
    std::atomic<std::uint64_t> commands_ignored_{0};
    std::atomic<std::uint64_t> commands_rejected_{0};
    std::atomic<std::uint64_t> publish_failures_{0};
    std::atomic<std::uint64_t> callback_failures_{0};

    mutable std::mutex error_mutex_;
    std::string last_error_;
};

}  // namespace dmxwb
