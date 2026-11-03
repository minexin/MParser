#include "mparser/cpp_api.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace mparser::sdk;
using Json = nlohmann::json;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

Value find(const std::vector<NamedValue>& variables, const std::string& name) {
    for (const auto& variable : variables) {
        if (variable.name == name) { return variable.value; }
    }
    throw std::runtime_error("missing variable " + name);
}

void requireChannels(const Json& actual, const Json& expected, const std::string& label) {
    require(actual.is_array() && actual.size() == expected.size(), label + " channel count");
    for (size_t index = 0; index < expected.size(); ++index) {
        // Allow only double-rounding, not a renderer-level color tolerance.
        const double value = expected.at(index).get<double>();
        const double error = std::abs(actual.at(index).get<double>() - value);
        require(error <= std::max(1e-30, std::abs(value) * 2e-15), label + " channel value");
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected color corpus path");
        std::ifstream file(argv[1]);
        const auto corpus = Json::parse(file);
        require(corpus.at("reference_release") == "2024b" && corpus.at("cases").size() == 75,
            "expected fixed 75-input R2024b reference corpus");
        size_t passed = 0;
        for (const auto backend : {Backend::Bytecode, Backend::Portable, Backend::Native, Backend::Automatic}) {
            for (const std::string target : {"Line", "Text"}) {
                for (const auto& test : corpus.at("cases")) {
                    const auto label = target + "/" + test.at("id").get<std::string>();
                    auto runtime = Runtime::create();
                    Invocation request;
                    request.backend = backend;
                    const auto source = target == "Line" ? "h=plot([1,2]);" : "plot([1,2]); h=title('Label');";
                    auto seed = runtime.execute(Module::compile(std::string(source) +
                        "h.Color=[0.2,0.3,0.4]; alias=h;", "color_seed.m"), request);
                    require(seed.succeeded(), label + " seed");
                    const auto frozen = seed.graphicsJson(3);
                    const auto previous = Json::parse(frozen);
                    request.initialWorkspace = seed.variables();
                    auto handle = find(request.initialWorkspace, "h");
                    const auto mutation = Module::compile("h.Color=" + test.at("expression").get<std::string>() +
                        "; result=h.Color;", "color_mutation.m");
                    require(mutation.isValid(), label + " input must compile");
                    auto result = runtime.execute(mutation, request);
                    const auto graph = Json::parse(result.graphicsJson(3));
                    const auto live = Json::parse(handle.graphicsJson(3));
                    require(graph.at("graphIdentity") == previous.at("graphIdentity"), label + " graph identity");
                    require(live.at("graph") == graph, label + " handle snapshot agreement");
                    require(seed.graphicsJson(3) == frozen, label + " immutable result snapshot");
                    require(result.succeeded() == test.at("accepted").get<bool>(), label + " accepted/rejected");
                    if (!result.succeeded()) {
                        require(graph == previous, label + " invalid write changed graph");
                        require(!result.diagnostics().empty(), label + " missing rejection diagnostic");
                        ++passed;
                        continue;
                    }
                    const auto values = result.variables();
                    const auto color = find(values, "result");
                    const auto reference = live.at("reference");
                    const auto alias = Json::parse(find(values, "alias").graphicsJson(3));
                    require(alias.at("reference") == reference && alias.at("graph") == graph, label + " alias identity");
                    for (size_t index = 0; index < graph.at("objects").size(); ++index) {
                        require(graph.at("objects")[index].at("objectIdentity") ==
                            previous.at("objects")[index].at("objectIdentity"), label + " object identity changed");
                    }
                    const auto node = graph.at("objects").at(reference.get<size_t>() - 1);
                    const auto& expected = test.at("value");
                    const auto& snapshotColor = node.at("properties").at("Color");
                    if (expected.is_string()) {
                        require(color.kind() == ValueKind::Character, label + " character getter");
                        const auto text = color.characterData();
                        const std::vector<std::uint16_t> units(text.begin(), text.end());
                        require(units == std::vector<std::uint16_t>{'n','o','n','e'} && snapshotColor == "none", label + " canonical none");
                    } else {
                        require(color.numericClass() == NumericClass::Double &&
                            color.dimensions() == std::vector<size_t>({1,3}), label + " RGB getter shape/type");
                        const auto channels = color.numericData<double>();
                        requireChannels(std::vector<double>(channels.begin(), channels.end()), expected, label);
                        requireChannels(snapshotColor, target == "Line" && test.contains("line_snapshot") ?
                            test.at("line_snapshot") : expected, label + " rendering snapshot");
                    }
                    const auto legacy = Json::parse(result.graphicsJson());
                    const bool needsV2 = target == "Text" || expected.is_string() || test.contains("line_snapshot");
                    require(legacy.at("version") == (needsV2 ? 2 : 1), label + " legacy schema selection");
                    ++passed;
                }
            }
        }
        std::cout << "PASS graphics Color: " << passed << " cases; normalization, aliases, immutable snapshots, invalid-write atomicity\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
