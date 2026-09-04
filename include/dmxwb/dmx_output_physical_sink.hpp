#pragma once

#include "dmxwb/dmx_output.hpp"
#include "dmxwb/instrumentation.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace dmxwb {

class DmxOutputBackend {
public:
    virtual ~DmxOutputBackend() = default;

    [[nodiscard]] virtual bool publish_snapshot(const DmxSnapshot& snapshot) = 0;
    [[nodiscard]] virtual bool start() = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    [[nodiscard]] virtual DmxOutputDiagnostics diagnostics() const = 0;
    [[nodiscard]] virtual DmxOutputOperationalState operational_state() const = 0;
};

class DmxOutputPhysicalSink final {
public:
    using BackendFactory = std::function<std::unique_ptr<DmxOutputBackend>(
        const DmxOutputConfig&,
        InstrumentationMode)>;

    DmxOutputPhysicalSink(
        DmxOutputConfig config,
        InstrumentationMode instrumentation_mode,
        BackendFactory backend_factory = {});

    DmxOutputPhysicalSink(const DmxOutputPhysicalSink&) = delete;
    DmxOutputPhysicalSink& operator=(const DmxOutputPhysicalSink&) = delete;
    DmxOutputPhysicalSink(DmxOutputPhysicalSink&&) = delete;
    DmxOutputPhysicalSink& operator=(DmxOutputPhysicalSink&&) = delete;

    [[nodiscard]] bool publish(const DmxSnapshot& snapshot);
    [[nodiscard]] bool reconfigure_port(std::string port);
    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] DmxOutputDiagnostics diagnostics() const;
    [[nodiscard]] DmxOutputOperationalState operational_state() const;

    [[nodiscard]] bool ever_started() const;
    [[nodiscard]] std::uint64_t start_failures() const;
    [[nodiscard]] std::uint64_t publish_failures() const;
    [[nodiscard]] std::uint64_t unexpected_stops() const;
    [[nodiscard]] std::uint64_t reconfigurations() const;
    [[nodiscard]] std::uint64_t reconfigure_failures() const;

private:
    void increment_engineering_counter(std::uint64_t& counter) noexcept;

    mutable std::mutex mutex_;
    DmxOutputConfig config_;
    InstrumentationMode instrumentation_mode_{InstrumentationMode::production};
    BackendFactory backend_factory_;
    std::unique_ptr<DmxOutputBackend> output_;
    std::optional<DmxSnapshot> latest_snapshot_;
    bool ever_started_{false};
    std::uint64_t start_failures_{0};
    std::uint64_t publish_failures_{0};
    std::uint64_t unexpected_stops_{0};
    std::uint64_t reconfigurations_{0};
    std::uint64_t reconfigure_failures_{0};
};

}  // namespace dmxwb
