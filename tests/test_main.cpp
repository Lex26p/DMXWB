#include "dmxwb/app_info.hpp"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect_equal(std::string_view actual, std::string_view expected, std::string_view test_name) {
    if (actual == expected) {
        std::cout << "[PASS] " << test_name << '\n';
        return;
    }

    ++failures;
    std::cerr << "[FAIL] " << test_name << ": expected '" << expected << "', got '" << actual << "'\n";
}

}  // namespace

int main() {
    expect_equal(dmxwb::application_name(), "dmxwb", "application name");
    expect_equal(dmxwb::application_version(), "0.1.0", "application version");

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All tests passed\n";
    return 0;
}
