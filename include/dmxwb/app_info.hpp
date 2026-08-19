#pragma once

#include <string_view>

namespace dmxwb {

[[nodiscard]] std::string_view application_name() noexcept;
[[nodiscard]] std::string_view application_version() noexcept;

}  // namespace dmxwb
