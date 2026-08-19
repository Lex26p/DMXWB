#include "dmxwb/monotonic_clock.hpp"

namespace dmxwb {

MonotonicClock::time_point SteadyMonotonicClock::now() const noexcept {
    return clock::now();
}

}  // namespace dmxwb
