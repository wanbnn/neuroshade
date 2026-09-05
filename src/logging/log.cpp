#include "logging/log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace neuroshade::logging {
namespace {

std::atomic current_level{Level::info};
std::mutex sink_mutex;
std::once_flag environment_sink_once;

void default_sink(Level message_level, std::string_view message) {
    std::clog << "[neuroshade] [" << to_string(message_level) << "] " << message << '\n';
}

Sink current_sink = default_sink;

}  // namespace

Level level_from_string(std::string_view value) noexcept {
    std::string normalized(value);
    std::ranges::transform(normalized, normalized.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

    if (normalized == "trace") return Level::trace;
    if (normalized == "debug") return Level::debug;
    if (normalized == "info") return Level::info;
    if (normalized == "warning" || normalized == "warn") return Level::warning;
    if (normalized == "error") return Level::error;
    if (normalized == "off") return Level::off;
    return Level::info;
}

std::string_view to_string(Level value) noexcept {
    switch (value) {
        case Level::trace: return "trace";
        case Level::debug: return "debug";
        case Level::info: return "info";
        case Level::warning: return "warning";
        case Level::error: return "error";
        case Level::off: return "off";
    }
    return "unknown";
}

void set_level(Level value) noexcept { current_level.store(value, std::memory_order_relaxed); }

Level level() noexcept { return current_level.load(std::memory_order_relaxed); }

void set_sink(Sink sink) {
    std::scoped_lock lock(sink_mutex);
    current_sink = sink ? std::move(sink) : Sink{default_sink};
}

void reset_sink() { set_sink(default_sink); }

void initialize_from_environment() {
    std::call_once(environment_sink_once, [] {
        const char* path = std::getenv("NEUROSHADE_LOG");
        if (path == nullptr || *path == '\0') return;
        auto stream = std::make_shared<std::ofstream>(path, std::ios::app);
        if (!*stream) return;
        set_sink([stream = std::move(stream)](Level message_level, std::string_view message) {
            default_sink(message_level, message);
            *stream << "[neuroshade] [" << to_string(message_level) << "] " << message << '\n';
            stream->flush();
        });
    });
}

void write(Level message_level, std::string_view message) {
    if (message_level < level() || level() == Level::off) return;
    std::scoped_lock lock(sink_mutex);
    current_sink(message_level, message);
}

}  // namespace neuroshade::logging
