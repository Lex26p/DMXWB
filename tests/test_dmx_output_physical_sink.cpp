#include "dmxwb/dmx_output_physical_sink.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

[[nodiscard]] dmxwb::DmxSnapshot make_snapshot(
    std::uint8_t first_channel,
    dmxwb::DmxSnapshot::Generation generation) {
    auto builder = dmxwb::DmxSnapshotBuilder::create(4);
    if (!builder.has_value()) {
        return {};
    }
    static_cast<void>(builder->set_channel(1, first_channel));
    const auto snapshot = builder->build(generation);
    return snapshot ? *snapshot : dmxwb::DmxSnapshot{};
}

struct FakeBackendState final {
    std::mutex mutex;
    std::condition_variable condition;
    std::string port;
    std::vector<dmxwb::DmxSnapshot> snapshots;
    bool block_publish{false};
    bool publish_entered{false};
    bool release_publish{false};
    bool running{false};
    std::uint64_t starts{0};
    std::uint64_t stops{0};
};

class FakeBackend final : public dmxwb::DmxOutputBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeBackendState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] bool publish_snapshot(const dmxwb::DmxSnapshot& snapshot) override {
        std::unique_lock lock{state_->mutex};
        state_->publish_entered = true;
        state_->condition.notify_all();
        state_->condition.wait(lock, [this] {
            return !state_->block_publish || state_->release_publish;
        });
        state_->snapshots.push_back(snapshot);
        return true;
    }

    [[nodiscard]] bool start() override {
        std::scoped_lock lock{state_->mutex};
        state_->running = true;
        ++state_->starts;
        return true;
    }

    void stop() noexcept override {
        std::scoped_lock lock{state_->mutex};
        state_->running = false;
        ++state_->stops;
    }

    [[nodiscard]] bool running() const noexcept override {
        std::scoped_lock lock{state_->mutex};
        return state_->running;
    }

    [[nodiscard]] dmxwb::DmxOutputDiagnostics diagnostics() const override {
        std::scoped_lock lock{state_->mutex};
        dmxwb::DmxOutputDiagnostics result;
        result.serial_open = state_->running;
        if (!state_->snapshots.empty()) {
            result.active_generation = state_->snapshots.back().generation();
        }
        return result;
    }

    [[nodiscard]] dmxwb::DmxOutputOperationalState operational_state() const override {
        std::scoped_lock lock{state_->mutex};
        dmxwb::DmxOutputOperationalState result;
        result.running = state_->running;
        result.serial_open = state_->running;
        if (!state_->snapshots.empty()) {
            result.slot_count = state_->snapshots.back().slot_count();
            result.active_generation = state_->snapshots.back().generation();
        }
        return result;
    }

private:
    std::shared_ptr<FakeBackendState> state_;
};

void test_publish_and_reconfigure_are_serialized() {
    std::vector<std::shared_ptr<FakeBackendState>> backends;
    auto factory = [&backends](
                       const dmxwb::DmxOutputConfig& config,
                       dmxwb::InstrumentationMode) {
        auto state = std::make_shared<FakeBackendState>();
        state->port = config.port;
        backends.push_back(state);
        return std::make_unique<FakeBackend>(std::move(state));
    };

    dmxwb::DmxOutputConfig config;
    config.port = "/dev/fake-1";
    dmxwb::DmxOutputPhysicalSink sink{
        config,
        dmxwb::InstrumentationMode::engineering,
        factory};

    expect_true(sink.publish(make_snapshot(10, 1)),
        "initial snapshot starts the physical output backend");
    expect_true(backends.size() == 1 && sink.running(),
        "initial backend is the only running output");

    const auto first_backend = backends.front();
    {
        std::scoped_lock lock{first_backend->mutex};
        first_backend->block_publish = true;
        first_backend->publish_entered = false;
        first_backend->release_publish = false;
    }

    bool publish_result = false;
    std::thread publisher{[&] {
        publish_result = sink.publish(make_snapshot(20, 2));
    }};

    bool publish_entered = false;
    {
        std::unique_lock lock{first_backend->mutex};
        publish_entered = first_backend->condition.wait_for(
            lock,
            std::chrono::seconds{2},
            [&] { return first_backend->publish_entered; });
    }
    expect_true(publish_entered,
        "test publication reached the old backend before reconfiguration");

    bool reconfigure_result = false;
    std::thread reconfigure{[&] {
        reconfigure_result = sink.reconfigure_port("/dev/fake-2");
    }};

    {
        std::scoped_lock lock{first_backend->mutex};
        first_backend->release_publish = true;
    }
    first_backend->condition.notify_all();

    publisher.join();
    reconfigure.join();

    expect_true(publish_result && reconfigure_result,
        "concurrent publication and port reconfiguration both complete");
    expect_true(backends.size() == 2,
        "port reconfiguration creates exactly one replacement backend");

    const auto replacement = backends.back();
    std::scoped_lock first_lock{first_backend->mutex};
    std::scoped_lock replacement_lock{replacement->mutex};
    expect_true(first_backend->stops == 1 && !first_backend->running,
        "old backend is stopped only after its in-flight publication completes");
    expect_true(replacement->starts == 1 && replacement->running,
        "replacement backend is started on the new port");
    expect_true(
        replacement->snapshots.size() == 1 &&
            replacement->snapshots.front().generation() == 2 &&
            replacement->snapshots.front().channel(1) == std::optional<std::uint8_t>{20},
        "replacement receives the latest completed whole snapshot without loss");
}

}  // namespace

int main() {
    test_publish_and_reconfigure_are_serialized();

    if (failures != 0) {
        std::cerr << failures << " physical DMX sink test(s) failed\n";
        return 1;
    }
    std::cout << "DMXWB physical DMX sink concurrency tests PASS\n";
    return 0;
}
