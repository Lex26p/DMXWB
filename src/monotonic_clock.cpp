#include "dmxwb/monotonic_clock.hpp"

#include <thread>

namespace dmxwb {

MonotonicClock::time_point SteadyMonotonicClock::now() const noexcept {
    return clock::now();
}

void SteadyMonotonicClock::sleep_until(time_point deadline) {
    std::this_thread::sleep_until(deadline);
}

}  // namespace dmxwb
