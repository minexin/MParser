#include "mparser/cpp_api.hpp"
#include <nlohmann/json.hpp>
#include <future>
#include <iostream>
#include <stdexcept>
#include <source_location>

using namespace mparser::sdk;
void require(bool value, const std::source_location location = std::source_location::current()) {
    if (!value) {
        throw std::runtime_error("graphics SDK invariant failed at line " + std::to_string(location.line()));
    }
}

int main() {
    try {
        Result firstPlot;
        std::string firstSnapshot;
        {
            auto runtime = Runtime::create();
            auto module = Module::compile("plot(1:3);", "no_output.m");
            firstPlot = runtime.execute(module);
            require(firstPlot.succeeded() && firstPlot.outputCount() == 0);
            firstSnapshot = firstPlot.graphicsJson();
            require(nlohmann::json::parse(firstSnapshot).at("objects").size() == 3);
            auto second = runtime.execute(module);
            require(second.succeeded());
            require(nlohmann::json::parse(second.graphicsJson()).at("objects").size() == 6);
            require(firstPlot.graphicsJson() == firstSnapshot);
            runtime.reset();
            auto empty = runtime.execute(Module::compile("x=1;", "empty.m"));
            require(empty.succeeded());
            require(nlohmann::json::parse(empty.graphicsJson()).at("objects").empty());
            require(firstPlot.graphicsJson() == firstSnapshot);
        }
        require(firstPlot.graphicsJson() == firstSnapshot);
        auto failed = Module::compile("plot(1:3); error('after plot');", "failure.m").execute();
        require(!failed.succeeded());
        require(nlohmann::json::parse(failed.graphicsJson()).at("objects").size() == 3);
        auto invalid = Module::compile("function out = broken(\n", "invalid.m").execute();
        require(!invalid.succeeded() && invalid.graphicsJson().empty());
        require(mparser_result_graphics_json(nullptr).size == 0);
        mparser_module* cModule = nullptr;
        mparser_result* cResult = nullptr;
        const std::string cSource = "plot(1:3);";
        require(mparser_module_compile_utf8(cSource.data(), cSource.size(), nullptr, 0, &cModule)
            == MPARSER_API_STATUS_OK);
        require(mparser_module_execute(cModule, nullptr, &cResult) == MPARSER_API_STATUS_OK);
        mparser_module_release(cModule);
        require(mparser_result_succeeded(cResult));
        const auto cSnapshot = mparser_result_graphics_json(cResult);
        require(nlohmann::json::parse(std::string(cSnapshot.data, cSnapshot.size))
            .at("objects").size() == 3);
        mparser_result_release(cResult);
        Value retained;
        Value retainedArray;
        {
            auto runtime = Runtime::create();
            auto module = Module::compile("h=plot([2,4,6]); h.DisplayName='series'; items=[h,h];", "graphics.m");
            auto result = runtime.execute(module);
            require(result.succeeded());
            for (const auto& variable : result.variables()) {
                if (variable.name == "h") { retained = variable.value; }
                if (variable.name == "items") { retainedArray = variable.value; }
            }
        }
        require(retained.hasValue());
        require(retainedArray.hasValue());
        const auto arraySnapshot = nlohmann::json::parse(retainedArray.graphicsJson());
        require(arraySnapshot["dimensions"] == nlohmann::json::array({1,2}));
        require(arraySnapshot["graphs"].size() == 1);
        require(arraySnapshot["elements"][0] == arraySnapshot["elements"][1]);
        const auto initial = nlohmann::json::parse(retained.graphicsJson());
        require(initial.at("valid") == true && initial.at("reference") == 3);
        require(initial.at("graph").at("objects").size() == 3);
        require(initial.at("graph").at("objects")[2]["properties"]["YData"] == nlohmann::json::array({2,4,6}));
        mparser_value* rawSnapshot = nullptr;
        require(mparser_value_graphics_json(nullptr, &rawSnapshot) == MPARSER_API_STATUS_INVALID_ARGUMENT);
        require(rawSnapshot == nullptr);
        require(mparser_value_graphics_json(nullptr, nullptr) == MPARSER_API_STATUS_INVALID_ARGUMENT);
        mparser_value* missing = nullptr;
        require(mparser_value_create_missing(&missing) == MPARSER_API_STATUS_OK);
        require(mparser_value_graphics_json(missing, &rawSnapshot) == MPARSER_API_STATUS_TYPE_MISMATCH);
        require(rawSnapshot == nullptr);
        mparser_value_release(missing);

        // An unrelated module can use the graph without its original owners.
        auto mutator = Module::compile("function change(h)\nh.Color=[1,0,0];\nend\n", "change.m");
        Invocation invocation;
        invocation.entryFunction = "change";
        invocation.arguments = {retained};
        invocation.requestedOutputCount = 0;
        auto reader = std::async(std::launch::async, [&] {
            for (int i = 0; i < 100; ++i) {
                const auto snapshot = nlohmann::json::parse(retained.graphicsJson());
                require(snapshot.at("valid") == true && snapshot.at("graph").at("objects").size() == 3);
            }
        });
        for (int i = 0; i < 30; ++i) { require(mutator.execute(invocation).succeeded()); }
        reader.get();
        const auto changed = nlohmann::json::parse(retained.graphicsJson());
        require(changed.at("graph").at("objects")[2]["properties"]["Color"] == nlohmann::json::array({1,0,0}));
        auto remover = Module::compile("function remove(h)\na=h.Parent; delete(a);\nend\n", "remove.m");
        invocation.entryFunction = "remove";
        require(remover.execute(invocation).succeeded());
        const auto deleted = nlohmann::json::parse(retained.graphicsJson());
        require(deleted.at("valid") == false && deleted.at("reference").is_null());
        require(deleted.at("graph").at("objects").size() == 1);
        const auto deletedArray = nlohmann::json::parse(retainedArray.graphicsJson());
        require(deletedArray["elements"][0]["valid"] == false &&
            deletedArray["elements"][1]["reference"].is_null());
        std::cout << "graphics SDK = retained,json,c-api,cross-module,concurrent,deleted\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
