#pragma once

#include "framegraph/pass/pass.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace neuroshade::framegraph {

inline constexpr std::uint32_t kPluginApiVersion = 1;

struct ExecutionPlan {
    std::vector<PassDescriptor> passes;
    std::size_t cache_key{};
};

struct CompileResult {
    ExecutionPlan plan;
    std::vector<std::string> errors;
    [[nodiscard]] bool valid() const noexcept { return errors.empty(); }
};

class Compiler {
public:
    [[nodiscard]] CompileResult compile(
        const std::vector<PassDescriptor>& passes,
        const std::unordered_map<std::string, ResourceFormat>& external_resources) const;
};

}  // namespace neuroshade::framegraph
