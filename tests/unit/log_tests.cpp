#include "logging/log.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace nslog = neuroshade::logging;

int main() {
    if (nslog::level_from_string("DEBUG") != nslog::Level::debug ||
        nslog::level_from_string("warn") != nslog::Level::warning ||
        nslog::level_from_string("invalid") != nslog::Level::info ||
        nslog::to_string(nslog::Level::error) != "error") {
        std::cerr << "level conversion failed\n";
        return 1;
    }

    std::vector<std::string> messages;
    nslog::set_sink([&messages](nslog::Level, std::string_view message) { messages.emplace_back(message); });
    nslog::set_level(nslog::Level::warning);
    nslog::write(nslog::Level::info, "filtered");
    nslog::write(nslog::Level::error, "retained");
    nslog::reset_sink();
    nslog::set_level(nslog::Level::info);

    if (messages.size() != 1 || messages.front() != "retained") {
        std::cerr << "log filtering failed\n";
        return 1;
    }

    return 0;
}
