#pragma once

#include <functional>
#include <string_view>

namespace neuroshade::logging {

enum class Level { trace, debug, info, warning, error, off };

using Sink = std::function<void(Level, std::string_view)>;

[[nodiscard]] Level level_from_string(std::string_view value) noexcept;
[[nodiscard]] std::string_view to_string(Level level) noexcept;

void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;
void set_sink(Sink sink);
void reset_sink();
// If NEUROSHADE_LOG is set, append messages to that per-session file while
// retaining stderr output. Safe to call repeatedly from loader entry points.
void initialize_from_environment();
void write(Level message_level, std::string_view message);

}  // namespace neuroshade::logging
