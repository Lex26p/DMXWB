#include "dmxwb/app_info.hpp"

#include <iostream>
#include <string_view>

namespace {

void print_help() {
    std::cout
        << "Usage: dmxwb [--help | --version]\n"
        << "\n"
        << "DEV-001 foundation executable. Hardware, MQTT and Art-Net runtime\n"
        << "subsystems are intentionally not enabled yet.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 1) {
        std::cout << dmxwb::application_name() << " " << dmxwb::application_version()
                  << " (DEV-001 foundation; no runtime subsystems enabled)\n";
        return 0;
    }

    if (argc == 2) {
        const std::string_view argument{argv[1]};

        if (argument == "--version") {
            std::cout << dmxwb::application_name() << " " << dmxwb::application_version() << '\n';
            return 0;
        }

        if (argument == "--help" || argument == "-h") {
            print_help();
            return 0;
        }
    }

    std::cerr << "Unknown arguments. Use --help for supported options.\n";
    return 2;
}
