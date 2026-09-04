#include "dmxwb/dmx_output_physical_sink.hpp"

#include <stdexcept>
#include <utility>

namespace dmxwb {
namespace {

class ProductionDmxOutputBackend final : public DmxOutputBackend {
public:
    ProductionDmxOutputBackend(
        const DmxOutputConfig& config,
        InstrumentationMode instrumentation_mode)
        : output_(config, instrumentation_mode) {}

    [[nodiscard]] bool publish_snapshot(const DmxSnapshot& snapshot) override {
        return output_.publish_snapshot(snapshot);
    }

    [[nodiscard]] bool start() override {
        return output_.start();
    }

    void stop() noexcept override {
        output_.stop();
    }

    [[nodiscard]] bool running() const noexcept override {
        return output_.running();
    }

    [[nodiscard]] DmxOutputDiagnostics diagnostics() const override {
        return output_.diagnostics();
    }

    [[nodiscard]] DmxOutputOperationalState operational_state() const override {
        return output_.operational_state();
    }

private:
    DmxOutput output_;
};

[[nodiscard]] std::unique_ptr<DmxOutputBackend> make_production_backend(
    const DmxOutputConfig& config,
    InstrumentationMode instrumentation_mode) {
    return std::make_unique<ProductionDmxOutputBackend>(config, instrumentation_mode);
}

}  // namespace

DmxOutputPhysicalSink::DmxOutputPhysicalSink(
    DmxOutputConfig config,
    InstrumentationMode instrumentation_mode,
    BackendFactory backend_factory)
    : config_(std::move(config)),
      instrumentation_mode_(instrumentation_mode),
      backend_factory_(std::move(backend_factory)) {
    if (!backend_factory_) {
        backend_factory_ = make_production_backend;
    }
    output_ = backend_factory_(config_, instrumentation_mode_);
    if (!output_) {
        throw std::invalid_argument("DmxOutput physical sink backend factory returned null");
    }
}

bool DmxOutputPhysicalSink::publish(const DmxSnapshot& snapshot) {
    std::scoped_lock lock{mutex_};

    if (!output_->publish_snapshot(snapshot)) {
        increment_engineering_counter(publish_failures_);
        return false;
    }
    latest_snapshot_ = snapshot;

    if (!output_->running()) {
        if (ever_started_) {
            increment_engineering_counter(unexpected_stops_);
            return false;
        }
        if (!output_->start()) {
            increment_engineering_counter(start_failures_);
            return false;
        }
        ever_started_ = true;
    }
    return true;
}

bool DmxOutputPhysicalSink::reconfigure_port(std::string port) {
    std::scoped_lock lock{mutex_};

    if (port == config_.port) {
        return true;
    }

    auto replacement_config = config_;
    replacement_config.port = std::move(port);

    std::unique_ptr<DmxOutputBackend> replacement;
    try {
        replacement = backend_factory_(replacement_config, instrumentation_mode_);
    } catch (...) {
        increment_engineering_counter(reconfigure_failures_);
        return false;
    }
    if (!replacement) {
        increment_engineering_counter(reconfigure_failures_);
        return false;
    }

    if (latest_snapshot_.has_value() &&
        !replacement->publish_snapshot(*latest_snapshot_)) {
        increment_engineering_counter(reconfigure_failures_);
        return false;
    }

    const bool was_running = output_->running();
    if (was_running) {
        output_->stop();
    }

    if (was_running && !replacement->start()) {
        if (!output_->start()) {
            increment_engineering_counter(unexpected_stops_);
        }
        increment_engineering_counter(reconfigure_failures_);
        return false;
    }

    output_ = std::move(replacement);
    config_ = std::move(replacement_config);
    increment_engineering_counter(reconfigurations_);
    return true;
}

void DmxOutputPhysicalSink::stop() {
    std::scoped_lock lock{mutex_};
    output_->stop();
}

bool DmxOutputPhysicalSink::running() const {
    std::scoped_lock lock{mutex_};
    return output_->running();
}

DmxOutputDiagnostics DmxOutputPhysicalSink::diagnostics() const {
    std::scoped_lock lock{mutex_};
    return output_->diagnostics();
}

DmxOutputOperationalState DmxOutputPhysicalSink::operational_state() const {
    std::scoped_lock lock{mutex_};
    return output_->operational_state();
}

bool DmxOutputPhysicalSink::ever_started() const {
    std::scoped_lock lock{mutex_};
    return ever_started_;
}

std::uint64_t DmxOutputPhysicalSink::start_failures() const {
    std::scoped_lock lock{mutex_};
    return start_failures_;
}

std::uint64_t DmxOutputPhysicalSink::publish_failures() const {
    std::scoped_lock lock{mutex_};
    return publish_failures_;
}

std::uint64_t DmxOutputPhysicalSink::unexpected_stops() const {
    std::scoped_lock lock{mutex_};
    return unexpected_stops_;
}

std::uint64_t DmxOutputPhysicalSink::reconfigurations() const {
    std::scoped_lock lock{mutex_};
    return reconfigurations_;
}

std::uint64_t DmxOutputPhysicalSink::reconfigure_failures() const {
    std::scoped_lock lock{mutex_};
    return reconfigure_failures_;
}

void DmxOutputPhysicalSink::increment_engineering_counter(
    std::uint64_t& counter) noexcept {
    if (engineering_instrumentation_enabled(instrumentation_mode_)) {
        ++counter;
    }
}

}  // namespace dmxwb
