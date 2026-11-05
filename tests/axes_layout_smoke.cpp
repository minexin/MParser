#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

using namespace mparser;
using Json = nlohmann::json;
void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}
void positionEquals(const std::vector<double>& actual, const Json& expected, const std::string& phase) {
    require(actual.size() == expected.size(), phase + " position shape");
    for (size_t index = 0; index < actual.size(); ++index) {
        const auto value = expected.at(index).get<double>();
        require(std::abs(actual[index] - value) <= 1e-10 * std::max(1.0, std::abs(value)),
            phase + " position " + std::to_string(index) + ": " + std::to_string(actual[index]));
    }
}
template<class Action> void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "expected invalid layout operation");
}
int main(int argc, char** argv) {
    try {
        require(argc == 3, "expected R2024b layout reference and v4 schema");
        std::ifstream input(argv[1]); const auto reference = Json::parse(input);
        std::ifstream schemaInput(argv[2]); nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(Json::parse(schemaInput));
        const auto& cases = reference.at("cases");
        const auto findCase = [&](const char* name) -> const Json& {
            const auto found = std::find_if(cases.begin(), cases.end(), [&](const auto& entry) {
                return entry.at("name") == name;
            });
            require(found != cases.end(), std::string("missing reference ") + name);
            return *found;
        };
        const auto& pixels = findCase("to_pixels").at("position");
        const auto& characters = findCase("to_characters").at("position");
        const RuntimeGraphicsLayoutEnvironment environment{800, 600,
            reference.at("pixels_per_inch").get<double>(),
            pixels.at(2).get<double>() / characters.at(2).get<double>(),
            pixels.at(3).get<double>() / characters.at(3).get<double>()};
        // Native Figure window sizing is not part of this headless layout slice.
        const std::set<std::string> nativeFigureCases{"resize_normalized", "resize_pixels", "reparent_normalized",
            "reparent_pixels", "figure_inches", "figure_zero_width", "figure_negative_width"};
        size_t checked = 0;
        const std::array modes{std::pair{true,ModuleExecutionBackend::Automatic},
            std::pair{false,ModuleExecutionBackend::Automatic},std::pair{false,ModuleExecutionBackend::Bytecode},
            std::pair{false,ModuleExecutionBackend::Portable},std::pair{false,ModuleExecutionBackend::Native}};
        for (const auto& [hir,backend] : modes) {
            for (const auto& entry : cases) {
                const auto name = entry.at("name").get<std::string>();
                if (nativeFigureCases.contains(name)) { continue; }
                auto state = std::make_shared<RuntimeSessionState>();
                const auto graph = state->graphicsGraph();
                const auto figure = graph->currentFigure();
                graph->setLayoutEnvironment(figure, environment);
                const auto axes = graph->currentAxes();
                const auto module = CompiledModule::compile("a=gca;" + entry.at("source").get<std::string>());
                require(module.valid(), name + " compilation");
                bool succeeded = false;
                if (hir) {
                    InterpreterOptions options; options.sessionState = state;
                    succeeded = Interpreter{}.run(module.semantic(),options).diagnostics.empty();
                } else {
                    ModuleInvocationRequest request; request.backend = backend;
                    succeeded = module.createSession(state).execute(request).succeeded();
                }
                require(succeeded == entry.at("succeeded").get<bool>(), name + (hir ? " HIR" : " VM") + " execution status");
                RuntimeGraphicsHandle handle(graph, axes);
                positionEquals(std::get<std::vector<double>>(handle.property("Position")), entry.at("position"), name);
                require(handle.property("Position") == handle.property("InnerPosition"), name + " InnerPosition alias");
                require(std::get<std::string>(handle.property("Units")) == entry.at("units").get<std::string>(), name + " units");
                require(std::get<std::string>(handle.property("XGrid")) == entry.at("grid").get<std::string>(), name + " grid reset");
                const auto member = runtimeGraphicsMember(makeRuntimeGraphicsValue(handle), "Position");
                require(member.succeeded && runtimeDimensions(member.value) == std::vector<size_t>({1, 4}), name + " row getter");
                ++checked;
            }
        }
        require(checked == 215, "43 source cases in HIR and four requested VM modes");
        auto graph = std::make_shared<RuntimeGraphicsGraph>();
        const auto figure = graph->currentFigure(); const auto axes = graph->currentAxes();
        require(!graph->layoutEnvironment(figure), "no guessed display metrics");
        validator.validate(Json::parse(graph->serialize(true, false, true)));
        auto before = graph->serialize(true);
        rejects([&] { graph->setProperty(axes, "Units", std::string("pixels")); });
        require(graph->serialize(true) == before, "missing environment mutated graph");
        graph->setProperty(axes, "Units", std::string("NORMAL"));
        graph->setLayoutEnvironment(figure, environment);
        auto frame = Json::parse(graph->serialize(true, false, true)); validator.validate(frame);
        require(frame.at("objects")[0].at("layout").at("revision") == "1", "initial environment revision");
        auto invalidFrame = frame; invalidFrame.at("objects")[0].erase("layout");
        bool schemaRejected = false;
        try { validator.validate(invalidFrame); } catch (const std::exception&) { schemaRejected = true; }
        require(schemaRejected, "v4 Figure requires explicit layout availability");
        for (const auto& malformed : {Json(0),Json(-1),Json("Inf")}) {
            invalidFrame = frame; invalidFrame.at("objects")[0].at("layout")["pixelsPerInch"] = malformed;
            schemaRejected = false;
            try { validator.validate(invalidFrame); } catch (const std::exception&) { schemaRejected = true; }
            require(schemaRejected, "v4 invalid metrics accepted");
        }
        for (const auto invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
            for (size_t field = 0; field < 5; ++field) {
                auto bad = environment;
                const std::array<double*, 5> fields{&bad.widthPixels, &bad.heightPixels, &bad.pixelsPerInch,
                    &bad.characterWidthPixels, &bad.characterHeightPixels};
                *fields.at(field) = invalid;
                rejects([&] { graph->setLayoutEnvironment(figure, bad); });
                require(graph->layoutEnvironment(figure)->widthPixels == environment.widthPixels,
                    "invalid environment changed current state");
            }
        }
        rejects([&] { graph->setLayoutEnvironment(axes, environment); });
        rejects([&] { (void)graph->layoutEnvironment(axes); });
        auto unrepresentable = environment; unrepresentable.pixelsPerInch = std::numeric_limits<double>::denorm_min();
        rejects([&] { graph->setLayoutEnvironment(figure, unrepresentable); });
        graph->setProperty(axes, "Position", std::vector<double>{0.1, 0.2, 0.4, 0.5});
        const RuntimeGraphicsSnapshot frozen(*graph); before = frozen.json(true);
        const auto frozenLayout = frozen.json(true, true);
        auto resized = environment; resized.widthPixels = 1000; resized.heightPixels = 500;
        graph->setLayoutEnvironment(figure, resized);
        require(frozen.json(true,true) == frozenLayout, "historical environment snapshot changed");
        graph->setProperty(axes, "Units", std::string("pixels"));
        RuntimeGraphicsHandle handle(graph, axes);
        positionEquals(std::get<std::vector<double>>(handle.property("Position")),
            findCase("resize_normalized").at("pixels"), "resized environment");
        const auto other = graph->create(RuntimeGraphicsKind::Figure);
        auto smaller = environment; smaller.widthPixels = 400; smaller.heightPixels = 300;
        graph->setLayoutEnvironment(other, smaller);
        graph->reparent(axes, other);
        positionEquals(std::get<std::vector<double>>(handle.property("Position")), Json::array({101, 101, 400, 250}),
            "reparent preserves current units");
        graph->setProperty(axes, "Units", std::string("normalized"));
        positionEquals(std::get<std::vector<double>>(handle.property("Position")), Json::array({0.25, 1.0/3, 1, 5.0/6}),
            "reparent uses new parent environment");
        graph->setProperty(axes, "InnerPosition", std::vector<double>{0, 0, 1, 1});
        positionEquals(std::get<std::vector<double>>(handle.property("Position")), Json::array({0, 0, 1, 1}), "alias setter");
        graph->setProperty(axes, "Position", std::vector<double>{0, 0, std::numeric_limits<double>::max(), 1});
        const auto overflowControl = graph->serialize(true);
        rejects([&] { graph->setProperty(axes, "Units", std::string("pixels")); });
        require(graph->serialize(true) == overflowControl && frozen.json(true) == before, "overflow/frozen state");
        auto huge = environment;
        huge.widthPixels = std::numeric_limits<double>::max();
        huge.heightPixels = std::numeric_limits<double>::max();
        huge.characterWidthPixels = std::numeric_limits<double>::max();
        huge.characterHeightPixels = std::numeric_limits<double>::max();
        graph->setLayoutEnvironment(other, huge);
        graph->setProperty(axes, "Position", std::vector<double>{0, 0, std::numeric_limits<double>::max(), 1});
        graph->setProperty(axes, "Units", std::string("characters"));
        require(std::get<std::vector<double>>(handle.property("Position"))[2] == std::numeric_limits<double>::max(),
            "representable conversion must not overflow its intermediate product");
        graph->erase(other);
        rejects([&] { graph->setLayoutEnvironment(other, environment); });
        std::cout << "PASS Axes layout: 215 R2024b HIR/VM source outcomes, explicit metrics, resize/reparent, aliases, overflow and frozen snapshots\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
