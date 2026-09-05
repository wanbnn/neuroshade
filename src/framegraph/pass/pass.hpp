#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace neuroshade::framegraph {

enum class PassType { capture, shader, neural, composite };
enum class QueuePreference { graphics, compute, either };
enum class ResourceFormat { unknown, rgba8_uint };

struct ResourceRequirement {
    std::string semantic;
    ResourceFormat format{ResourceFormat::unknown};
    bool mandatory{true};
};

struct ResourceOutput {
    std::string semantic;
    ResourceFormat format{ResourceFormat::unknown};
};

struct PassDescriptor {
    std::string id;
    PassType type{PassType::shader};
    std::vector<ResourceRequirement> inputs;
    std::vector<ResourceOutput> outputs;
    QueuePreference queue{QueuePreference::either};
    std::uint32_t plugin_api{1};
};

}  // namespace neuroshade::framegraph
