#pragma once

#include <chrono>

namespace dmxwb {

class MonotonicClock {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;
    using duration = clock::duration;

    virtual ~MonotonicClock() = default;

    [[nodiscard]] virtual time_point now() const noexcept = 0;
    virtual void sleep_until(time_point deadline) = 0;
};

class SteadyMonotonicClock final : public MonotonicClock {
public:
    [[nodiscard]] time_point now() const noexcept override;
    void sleep_until(time_point deadline) override;
};

}  // namespace dmxwb
