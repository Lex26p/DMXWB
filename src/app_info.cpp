#include "dmxwb/app_info.hpp"

#ifndef DMXWB_VERSION
#define DMXWB_VERSION "unknown"
#endif

namespace dmxwb {

std::string_view application_name() noexcept {
    return "dmxwb";
}

std::string_view application_version() noexcept {
    return DMXWB_VERSION;
}

}  // namespace dmxwb
