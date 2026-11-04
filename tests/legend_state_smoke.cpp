#include "mparser/cpp_api.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace mparser::sdk;
using Json = nlohmann::json;
void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}
std::string text(const Value& value) {
    const auto units = value.characterData();
    std::string result;
    for (const auto unit : units) {
        require(unit <= 127, "ASCII reference label expected");
        result.push_back(static_cast<char>(unit));
    }
    return result;
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected R2024b Legend reference");
        std::ifstream input(argv[1]); const auto reference = Json::parse(input);
        const std::pair<const char*, const char*> phases[] = {
            {"initial", "a=plot(1:2); hold on; b=plot(2:3); g=legend;"},
            {"off_added", "g.AutoUpdate='off'; c=plot(3:4); c.DisplayName='third';"},
            {"off_rename", "a.DisplayName='renamed';"},
            {"off_deleted", "delete(a);"},
            {"on_again", "g.AutoUpdate='on';"},
            {"on_added", "d=plot(4:5);"},
            {"labels", "g.String={'custom B','custom C'};"},
            {"after_labels", "e=plot(5:6);"},
            {"short_labels", "g.String={'only B'};"},
            {"extra_labels", "g.String={'B','D','E','extra'};"},
            {"empty_name", "b.DisplayName='';"},
            {"empty_labels", "g.String={};"},
            {"mixed_names", "close all; a=plot(1:2); a.DisplayName='measured'; hold on; b=plot(2:3); g=legend;"},
            {"mixed_added", "c=plot(3:4);"}
        };
        for (const auto backend : {Backend::Automatic, Backend::Bytecode, Backend::Portable, Backend::Native}) {
            auto runtime = Runtime::create(); Invocation request; request.backend = backend;
            Result retained; std::string frozen;
            for (const auto& [phase, source] : phases) {
                const auto result = runtime.execute(Module::compile(std::string(source)+" labels=g.String; mode=g.AutoUpdate;", "legend_state.m"), request);
                require(result.succeeded(), std::string(phase)+" execution");
                const auto graph = Json::parse(result.graphicsJson(3));
                Json labels = Json::array(), names = Json::array();
                const auto& expected = reference.at(phase);
                for (const auto& node : graph.at("objects")) {
                    if (node.at("type") == "Legend") {
                        for (const auto& entry : node.at("entries")) { labels.push_back(entry.at("label")); }
                        require(node.at("properties").at("AutoUpdate") == expected.at("auto"), std::string(phase)+" auto");
                    }
                    if (node.at("type") == "Line") { names.push_back(node.at("properties").at("DisplayName")); }
                }
                require(labels == expected.at("labels"), std::string(phase)+" labels "+labels.dump());
                require(names == expected.at("names"), std::string(phase)+" names "+names.dump());
                for (const auto& variable : result.variables()) {
                    if (variable.name == "labels") {
                        require(variable.value.dimensions() == std::vector<size_t>({1, labels.size()}), "String cell row");
                        for (size_t index=0;index<labels.size();++index) {
                            require(text(variable.value.cellElement(index)) == labels[index].get<std::string>(), "String getter");
                        }
                    }
                }
                if (!retained) { retained = result; frozen = result.graphicsJson(3); }
                else { require(retained.graphicsJson(3) == frozen, "old Legend snapshot mutated"); }
                request.initialWorkspace = result.variables();
            }
            const auto before = runtime.execute(Module::compile("g.AutoUpdate=false;", "legend_before.m"), request);
            require(before.succeeded(), "boolean AutoUpdate");
            request.initialWorkspace = before.variables();
            const auto snapshot = before.graphicsJson(3);
            for (const auto source : {"g.AutoUpdate=2;", "g.AutoUpdate='invalid';", "g.String={'changed',42};"}) {
                const auto denied = runtime.execute(Module::compile(source,"legend_invalid.m"),request);
                require(!denied.succeeded() && denied.graphicsJson(3) == snapshot, "invalid write changed graph");
            }
            const auto strings = runtime.execute(Module::compile("g.String=[\"string label\",\"ignored\"];", "legend_strings.m"), request);
            require(strings.succeeded(), "string-array labels");
        }
        std::cout << "PASS Legend: 14 R2024b phases in four requested modes, frozen snapshots and invalid-write atomicity\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
