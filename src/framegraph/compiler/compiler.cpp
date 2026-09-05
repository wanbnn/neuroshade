#include "framegraph/compiler/compiler.hpp"

#include <algorithm>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_set>

namespace neuroshade::framegraph {
namespace {

void hash_combine(std::size_t& seed, std::string_view value) {
    constexpr std::size_t prime = sizeof(std::size_t) == 8 ? 1099511628211ULL : 16777619U;
    if (seed == 0) seed = sizeof(std::size_t) == 8 ? 14695981039346656037ULL : 2166136261U;
    for (const unsigned char character : value) {
        seed ^= character;
        seed *= prime;
    }
    seed ^= 0xffU;
    seed *= prime;
}

}  // namespace

CompileResult Compiler::compile(
    const std::vector<PassDescriptor>& passes,
    const std::unordered_map<std::string, ResourceFormat>& external_resources) const {
    CompileResult result;
    std::unordered_map<std::string, std::size_t> producers;
    std::unordered_set<std::string> pass_ids;

    for (std::size_t index = 0; index < passes.size(); ++index) {
        const auto& pass = passes[index];
        if (pass.id.empty() || !pass_ids.emplace(pass.id).second) {
            result.errors.emplace_back("duplicate or empty pass id: " + pass.id);
        }
        if (pass.plugin_api != kPluginApiVersion) {
            result.errors.emplace_back("pass " + pass.id + " uses unsupported plugin API " +
                                       std::to_string(pass.plugin_api));
        }
        for (const auto& output : pass.outputs) {
            if (output.semantic.empty()) {
                result.errors.emplace_back("pass " + pass.id + " declares an empty output semantic");
            } else if (!producers.emplace(output.semantic, index).second) {
                result.errors.emplace_back("multiple writers for resource " + output.semantic);
            }
        }
    }

    std::vector<std::vector<std::size_t>> edges(passes.size());
    std::vector<std::size_t> indegree(passes.size());
    for (std::size_t consumer = 0; consumer < passes.size(); ++consumer) {
        for (const auto& input : passes[consumer].inputs) {
            const auto external = external_resources.find(input.semantic);
            const auto producer = producers.find(input.semantic);
            if (producer == producers.end() && external == external_resources.end()) {
                if (input.mandatory) {
                    result.errors.emplace_back("pass " + passes[consumer].id +
                                               " is missing mandatory input " + input.semantic);
                }
                continue;
            }
            ResourceFormat actual = ResourceFormat::unknown;
            if (producer != producers.end()) {
                const auto& outputs = passes[producer->second].outputs;
                const auto found = std::ranges::find(outputs, input.semantic, &ResourceOutput::semantic);
                if (found != outputs.end()) actual = found->format;
                if (producer->second != consumer) {
                    edges[producer->second].push_back(consumer);
                    ++indegree[consumer];
                } else {
                    result.errors.emplace_back("pass " + passes[consumer].id + " reads its own output " + input.semantic);
                }
            } else {
                actual = external->second;
            }
            if (input.format != ResourceFormat::unknown && actual != ResourceFormat::unknown &&
                input.format != actual) {
                result.errors.emplace_back("incompatible format for " + input.semantic + " consumed by " +
                                           passes[consumer].id);
            }
        }
    }

    if (!result.errors.empty()) return result;

    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    for (std::size_t index = 0; index < indegree.size(); ++index) {
        if (indegree[index] == 0) ready.push(index);
    }
    while (!ready.empty()) {
        const auto index = ready.top();
        ready.pop();
        result.plan.passes.push_back(passes[index]);
        for (const auto dependent : edges[index]) {
            if (--indegree[dependent] == 0) ready.push(dependent);
        }
    }
    if (result.plan.passes.size() != passes.size()) {
        result.errors.emplace_back("framegraph contains a non-temporal cycle");
        result.plan.passes.clear();
        return result;
    }

    std::size_t key = 0;
    for (const auto& pass : result.plan.passes) {
        hash_combine(key, pass.id);
        for (const auto& input : pass.inputs) hash_combine(key, input.semantic);
        for (const auto& output : pass.outputs) hash_combine(key, output.semantic);
    }
    result.plan.cache_key = key;
    return result;
}

}  // namespace neuroshade::framegraph
