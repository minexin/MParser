#include "mparser/cpp_api.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

using namespace mparser::sdk;
using Json = nlohmann::json;
void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}
template<class Action> void rejects(Action action, ApiStatus status) {
    try { action(); } catch (const ApiError& error) { require(error.status() == status, "unexpected layout API status"); return; }
    throw std::runtime_error("expected layout API rejection");
}
const Json& node(const Json& graph, const char* type) {
    const auto& objects = graph.at("objects");
    const auto found = std::find_if(objects.begin(), objects.end(), [&](const auto& candidate) { return candidate.at("type") == type; });
    require(found != objects.end(), std::string("missing ") + type); return *found;
}
void positionEquals(const Json& actual, const Json& expected, const std::string& name) {
    require(actual.size() == expected.size(), name + " position size");
    for (size_t index = 0; index < actual.size(); ++index) {
        const auto value = expected.at(index).get<double>();
        require(std::abs(actual.at(index).get<double>() - value) <= 1e-10 * std::max(1.0, std::abs(value)), name + " position");
    }
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected R2024b layout reference");
        std::ifstream input(argv[1]); const auto reference = Json::parse(input);
        const auto& cases = reference.at("cases");
        const auto findCase = [&](const char* name) -> const Json& {
            const auto found = std::find_if(cases.begin(), cases.end(), [&](const auto& entry) { return entry.at("name") == name; });
            require(found != cases.end(), "missing reference"); return *found;
        };
        const auto& pixels = findCase("to_pixels").at("position");
        const auto& characters = findCase("to_characters").at("position");
        const GraphicsLayoutEnvironment environment{800,600,reference.at("pixels_per_inch").get<double>(),
            pixels.at(2).get<double>() / characters.at(2).get<double>(),
            pixels.at(3).get<double>() / characters.at(3).get<double>()};
        const std::set<std::string> nativeFigureCases{"resize_normalized","resize_pixels","reparent_normalized",
            "reparent_pixels","figure_inches","figure_zero_width","figure_negative_width"};
        size_t checked = 0;
        for (const auto backend : {Backend::Automatic,Backend::Bytecode,Backend::Portable,Backend::Native}) {
            for (const auto& entry : cases) {
                const auto name = entry.at("name").get<std::string>(); if (nativeFigureCases.contains(name)) { continue; }
                auto runtime = Runtime::create(); Invocation request; request.backend = backend;
                const auto seed = runtime.execute(Module::compile("a=axes;","layout_seed.m"),request);
                require(seed.succeeded(), "layout seed"); const auto frozenSeed = seed.graphicsJson(4);
                const auto initial = Json::parse(runtime.graphicsJson(4));
                require(node(initial,"Figure").at("layout").is_null(), "no implicit metrics");
                runtime.setGraphicsLayoutEnvironment(initial.at("graphIdentity").get<std::string>(),
                    node(initial,"Figure").at("objectIdentity").get<std::string>(),environment);
                const auto result = runtime.execute(Module::compile("a=gca;" + entry.at("source").get<std::string>(),"layout_case.m"),request);
                require(result.succeeded() == entry.at("succeeded").get<bool>(), name + " status");
                const auto graph = Json::parse(result.graphicsJson(4));
                require(graph.at("version") == 4, "v4 result");
                const auto& axes = node(graph,"Axes").at("properties");
                positionEquals(axes.at("Position"),entry.at("position"),name);
                require(axes.at("Units") == entry.at("units") && axes.at("XGrid") == entry.at("grid"), name + " properties");
                const auto& layout = node(graph,"Figure").at("layout");
                require(layout.at("revision") == "1" && layout.at("contentSizePixels") == Json::array({800,600}), name + " environment");
                require(seed.graphicsJson(4) == frozenSeed, "historical result gained environment");
                require(!node(Json::parse(runtime.graphicsJson(3)),"Figure").contains("layout"), "v3 layout leak");
                const auto object = runtime.graphicsObject(graph.at("graphIdentity").get<std::string>(),node(graph,"Axes").at("objectIdentity").get<std::string>());
                require(Json::parse(object.graphicsJson(4)).at("graph") == graph, "value v4 graph");
                ++checked;
            }
        }
        require(checked == 172, "43 public SDK cases in four requested modes");
        auto runtime = Runtime::create(); const auto seed = runtime.execute(Module::compile("h=plot(1:3); a=gca;","layout_lifecycle.m"));
        require(seed.succeeded(), "lifecycle seed"); const auto initial = Json::parse(runtime.graphicsJson(4));
        const auto graph = initial.at("graphIdentity").get<std::string>();
        const auto figure = node(initial,"Figure").at("objectIdentity").get<std::string>();
        const auto axes = node(initial,"Axes").at("objectIdentity").get<std::string>();
        runtime.setGraphicsLayoutEnvironment(graph,figure,environment);
        const auto capture = runtime.execute(Module::compile("marker=42;","layout_capture.m"));
        require(capture.succeeded(), "capture"); const auto frozen = capture.graphicsJson(4);
        const auto current = runtime.graphicsJson(4);
        for (const auto invalid : {0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
            auto bad = environment; bad.widthPixels = invalid;
            rejects([&] { runtime.setGraphicsLayoutEnvironment(graph,figure,bad); },MPARSER_API_STATUS_INVALID_ARGUMENT);
        }
        rejects([&] { runtime.setGraphicsLayoutEnvironment(graph,axes,environment); },MPARSER_API_STATUS_TYPE_MISMATCH);
        rejects([&] { runtime.setGraphicsLayoutEnvironment(graph,"01",environment); },MPARSER_API_STATUS_INVALID_ARGUMENT);
        rejects([&] { runtime.setGraphicsLayoutEnvironment(graph,"18446744073709551616",environment); },MPARSER_API_STATUS_INVALID_ARGUMENT);
        auto foreign = Runtime::create();
        rejects([&] { foreign.setGraphicsLayoutEnvironment(graph,figure,environment); },MPARSER_API_STATUS_OWNER_MISMATCH);
        require(runtime.graphicsJson(4) == current, "invalid environment advanced revision");
        auto writer = std::async(std::launch::async,[runtime,graph,figure,environment]() mutable {
            for (int index = 0; index < 100; ++index) {
                auto resized = environment; resized.widthPixels = 500 + index; resized.heightPixels = 2 * resized.widthPixels;
                runtime.setGraphicsLayoutEnvironment(graph,figure,resized);
            }
        });
        for (int index = 0; index < 100; ++index) {
            const auto frame = Json::parse(runtime.graphicsJson(4)); const auto& layout = node(frame,"Figure").at("layout");
            const auto revision = std::stoull(layout.at("revision").get<std::string>());
            if (revision > 1) {
                require(layout.at("contentSizePixels")[1].get<double>() == 2 * layout.at("contentSizePixels")[0].get<double>(), "torn environment read");
            }
        }
        writer.get(); require(capture.graphicsJson(4) == frozen, "historical environment changed");
        const auto retained = runtime.graphicsObject(graph,figure);
        auto resetWriter = std::async(std::launch::async,[runtime,graph,figure,environment]() mutable {
            for (int index = 0; index < 100; ++index) {
                try { runtime.setGraphicsLayoutEnvironment(graph,figure,environment); }
                catch (const ApiError& error) { require(error.status() == MPARSER_API_STATUS_OWNER_MISMATCH,"reset environment race"); }
            }
        });
        runtime.reset(); resetWriter.get();
        rejects([&] { runtime.setGraphicsLayoutEnvironment(graph,figure,environment); },MPARSER_API_STATUS_OWNER_MISMATCH);
        require(Json::parse(runtime.graphicsJson(4)).at("objects").empty(), "stale metrics changed new graph");
        require(Json::parse(retained.graphicsJson(4)).at("valid") == true, "retained old graph control");
        auto deleted = Runtime::create(); const auto beforeDelete = deleted.execute(Module::compile("f=figure;","layout_delete.m"));
        require(beforeDelete.succeeded(), "delete seed"); const auto deleteGraph = Json::parse(deleted.graphicsJson(4));
        Invocation request; request.initialWorkspace = beforeDelete.variables();
        require(deleted.execute(Module::compile("close(f);","layout_close.m"),request).succeeded(),"close Figure");
        rejects([&] { deleted.setGraphicsLayoutEnvironment(deleteGraph.at("graphIdentity").get<std::string>(),
            node(deleteGraph,"Figure").at("objectIdentity").get<std::string>(),environment); },MPARSER_API_STATUS_OUT_OF_RANGE);
        mparser_runtime* raw = nullptr; require(mparser_runtime_create(nullptr,&raw) == MPARSER_API_STATUS_OK,"C runtime");
        mparser_graphics_layout_environment c{0,MPARSER_C_ABI_GENERATION,800,600,96,6,14};
        require(mparser_runtime_graphics_set_layout_environment(raw,{"1",1},{"1",1},&c) == MPARSER_API_STATUS_ABI_MISMATCH,"C small structure");
        c.struct_size = MPARSER_GRAPHICS_LAYOUT_ENVIRONMENT_SIZE; c.abi_generation = 99;
        require(mparser_runtime_graphics_set_layout_environment(raw,{"1",1},{"1",1},&c) == MPARSER_API_STATUS_ABI_MISMATCH,"C wrong generation");
        require(mparser_runtime_graphics_set_layout_environment(raw,{"1",1},{"1",1},nullptr) == MPARSER_API_STATUS_INVALID_ARGUMENT,"C null environment");
        mparser_runtime_release(raw);
        std::cout << "PASS public layout environment SDK: 172 R2024b outcomes, frozen v4, legacy isolation, invalid/stale targets and reset/concurrent profile reads\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
