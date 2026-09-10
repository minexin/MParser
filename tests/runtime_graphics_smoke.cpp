#include "mparser/runtime/core/object_model/runtime_graphics.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/embedding/machine_protocol.h"
#include "mparser/execution/interpreter.h"
#include "mparser/runtime/builtins/builtin_registry.h"
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>
#include <fstream>
#include <future>

using namespace mparser;
void require(bool value) { if (!value) { throw std::runtime_error("graphics graph invariant failed"); } }
int main(int argc, char** argv) {
    try {
        require(argc == 3);
        std::ifstream schemaInput(argv[1]);
        nlohmann::json schema;
        schemaInput >> schema;
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(schema);
        std::ifstream historicalSchemaInput(argv[2]);
        nlohmann::json historicalSchema;
        historicalSchemaInput >> historicalSchema;
        nlohmann::json_schema::json_validator historicalValidator;
        historicalValidator.set_root_schema(historicalSchema);
        for (bool hir : {false, true}) {
            auto module = CompiledModule::compile(
                "a=plot(1:3); b=plot(3:-1:1); items=[a,b;a,b]; ax=a.Parent;"
                "b.Parent=ax; children=ax.Children; delete(a);");
            require(module.valid());
            std::vector<RuntimeVariable> variables;
            if (hir) {
                auto result = Interpreter{}.run(module.semantic());
                require(result.diagnostics.empty());
                variables = std::move(result.variables);
            } else {
                auto result = module.execute();
                require(result.succeeded());
                variables = std::move(result.variables);
            }
            bool foundItems = false;
            bool foundChildren = false;
            for (const auto& variable : variables) {
                if (variable.name != "items" && variable.name != "children") { continue; }
                const auto snapshot = nlohmann::json::parse(serializeRuntimeGraphicsValue(variable.value));
                require(snapshot.at("graphs").size() == 1);
                const auto& elements = snapshot.at("elements");
                require(elements.at(0).at("valid") == false && elements.at(0).at("reference").is_null());
                if (variable.name == "items") {
                    foundItems = true;
                    require(snapshot.at("dimensions") == nlohmann::json::array({2,2}));
                    require(elements.size() == 4 && elements[0] == elements[1] && elements[2] == elements[3]);
                    require(elements[2].at("valid") == true);
                } else {
                    foundChildren = true;
                    require(snapshot.at("dimensions") == nlohmann::json::array({2,1}));
                    require(elements.size() == 2 && elements[1].at("valid") == true);
                }
                ModuleInvocationResult exported;
                exported.outputs.push_back(variable.value);
                auto wire = nlohmann::json::parse(serializeMachineResultJsonV1(exported, "test"));
                validator.validate(wire);
                require(wire["outputs"][0]["value"]["representation"] == "graphics-array");
                require(wire["outputs"][0]["value"]["graphics"] == snapshot);
                wire["outputs"][0]["value"]["graphics"]["elements"][0]["reference"] = 1;
                bool rejected = false;
                try { validator.validate(wire); } catch (const std::exception&) { rejected = true; }
                require(rejected);
            }
            require(foundItems && foundChildren);
        }
        {
            auto first = std::make_shared<RuntimeGraphicsGraph>();
            auto second = std::make_shared<RuntimeGraphicsGraph>();
            const auto a = makeRuntimeGraphicsValue(RuntimeGraphicsHandle(first, first->plot({1,2})));
            const auto b = makeRuntimeGraphicsValue(RuntimeGraphicsHandle(second, second->plot({3,4})));
            const auto array = runtimeMakeObjectArrayFromLogicalOrder({a,b,a}, {1,3}, a.className, true);
            require(array.succeeded);
            const auto snapshot = nlohmann::json::parse(serializeRuntimeGraphicsValue(array.value));
            require(snapshot.at("graphs").size() == 2);
            require(snapshot["elements"][0] == snapshot["elements"][2]);
            require(snapshot["elements"][0]["graph"] == 1 && snapshot["elements"][1]["graph"] == 2);
            const auto empty = runtimeMakeObjectArrayFromLogicalOrder({}, {0,2}, a.className, true);
            require(empty.succeeded);
            const auto emptySnapshot = nlohmann::json::parse(serializeRuntimeGraphicsValue(empty.value));
            require(emptySnapshot["dimensions"] == nlohmann::json::array({0,2}));
            require(emptySnapshot["elements"].empty() && emptySnapshot["graphs"].empty());
            auto ordinary = a;
            ordinary.graphicsHandle.reset();
            require(!isRuntimeGraphicsValue(ordinary));
            auto malformed = array.value;
            malformed.dimensions = {std::numeric_limits<size_t>::max(), 2};
            bool rejected = false;
            try { (void)serializeRuntimeGraphicsValue(malformed); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected);
            const auto reverse = runtimeMakeObjectArrayFromLogicalOrder({b,a,b}, {1,3}, a.className, true);
            require(reverse.succeeded);
            auto reader = std::async(std::launch::async, [&] {
                for (int i = 0; i < 100; ++i) {
                    require(nlohmann::json::parse(serializeRuntimeGraphicsValue(array.value)) == snapshot);
                }
            });
            for (int i = 0; i < 100; ++i) { (void)serializeRuntimeGraphicsValue(reverse.value); }
            reader.get();
        }
        std::string referenceSnapshot;
        for (bool hir : {false, true}) {
            auto state = std::make_shared<RuntimeSessionState>();
            auto module = CompiledModule::compile("plot(1:3);");
            ModuleInvocationResult exported;
            if (hir) {
                InterpreterOptions options;
                options.sessionState = state;
                auto result = Interpreter{}.run(module.semantic(), options);
                require(result.diagnostics.empty());
                exported.graphicsSnapshot = std::move(result.graphicsSnapshot);
            } else {
                exported = module.createSession(state).execute();
                require(exported.succeeded() && exported.outputs.empty());
            }
            require(exported.graphicsSnapshot.has_value());
            const auto snapshot = exported.graphicsSnapshot->json();
            if (referenceSnapshot.empty()) { referenceSnapshot = snapshot; }
            require(snapshot == referenceSnapshot);
            auto wire = nlohmann::json::parse(serializeMachineResultJsonV1(exported, "test"));
            validator.validate(wire);
            require(wire.at("graphics").at("objects").size() == 3);
            wire["graphics"]["objects"][0]["type"] = "Unknown";
            bool rejected = false;
            try { validator.validate(wire); } catch (const std::exception&) { rejected = true; }
            require(rejected);
            state->reset();
            require(exported.graphicsSnapshot->json() == snapshot);
            require(state->graphicsGraph()->nodes().empty());
            const auto invalidFigure = state->graphicsGraph()->create(RuntimeGraphicsKind::Figure);
            state->graphicsGraph()->setProperty(invalidFigure, "Name", std::string(1, '\xff'));
            const auto emptyModule = CompiledModule::compile("x=1;");
            bool exportFailed = false;
            if (hir) {
                InterpreterOptions options;
                options.sessionState = state;
                const auto result = Interpreter{}.run(emptyModule.semantic(), options);
                require(!result.graphicsSnapshot);
                for (const auto& diagnostic : result.diagnostics) {
                    exportFailed |= diagnostic.identifier == "MParser:GraphicsExportFailed";
                }
            } else {
                const auto result = emptyModule.createSession(state).execute();
                require(!result.succeeded() && !result.graphicsSnapshot);
                for (const auto& diagnostic : result.diagnostics) {
                    exportFailed |= diagnostic.identifier == "MParser:GraphicsExportFailed";
                }
            }
            require(exportFailed);
        }
        for (bool hir : {false, true}) {
            auto session = std::make_shared<RuntimeSessionState>();
            const auto module = CompiledModule::compile(
                "h=plot([3,5,8]); alias=h; k=plot([2;4],[7;9]); plot([]); same=isequal(h,alias); different=isequal(h,k);"
                "ax=h.Parent; fig=ax.Parent; children=ax.Children; sameChild=isequal(children,h);"
                "alias.Color=[1,0,0]; color=h.Color; fig.Name='headless'; name=fig.Name;"
                "otherAx=k.Parent; h.Parent=otherAx; moved=isequal(h.Parent,otherAx);"
                "doomed=plot([4,5]); doomedAlias=doomed; doomedAx=doomed.Parent;"
                "before=isvalid(doomed); delete(doomedAx); after=isvalid(doomedAlias);");
            require(module.valid());
            std::vector<RuntimeVariable> variables;
            if (hir) {
                InterpreterOptions options;
                options.sessionState = session;
                const auto result = Interpreter{}.run(module.semantic(), options);
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.diagnostics.empty());
                variables = result.variables;
            } else {
                const auto result = module.createSession(session).execute();
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.succeeded());
                variables = result.variables;
            }
            require(session->graphicsGraph()->nodes().size() == 10);
            RuntimeValue retained;
            for (const auto& variable : variables) {
                if (variable.name == "h") { retained = variable.value; }
                if (variable.name == "same") { require(variable.value.number == 1); }
                if (variable.name == "different") { require(variable.value.number == 0); }
                if (variable.name == "before") { require(variable.value.number == 1); }
                if (variable.name == "after") { require(variable.value.number == 0); }
                if (variable.name == "sameChild" || variable.name == "moved") { require(variable.value.number == 1); }
                if (variable.name == "color") { require(variable.value.elements == std::vector<double>{1,0,0}); }
            }
            require(retained.graphicsHandle && retained.graphicsHandle->valid());
            ModuleInvocationResult exported;
            exported.outputs.push_back(retained);
            const auto wire = nlohmann::json::parse(serializeMachineResultJsonV1(exported, "test"));
            validator.validate(wire);
            require(wire.at("protocol").at("minor") == 2);
            bool rejectedByHistoricalSchema = false;
            try { historicalValidator.validate(wire); }
            catch (const std::exception&) { rejectedByHistoricalSchema = true; }
            require(rejectedByHistoricalSchema);
            auto invalidWire = wire;
            invalidWire["outputs"][0]["value"]["graphics"]["graph"]["objects"][0]["type"] = "Unknown";
            bool rejectedWire = false;
            try { validator.validate(invalidWire); } catch (const std::exception&) { rejectedWire = true; }
            require(rejectedWire);
            const auto& object = wire.at("outputs").at(0).at("value");
            require(object.at("representation") == "graphics");
            const auto& snapshot = object.at("graphics");
            require(snapshot.at("valid") == true && snapshot.at("reference") == 3);
            require(snapshot.at("graph").at("objects").size() == 10);
            require(std::get<std::vector<double>>(retained.graphicsHandle->property("XData")) == std::vector<double>{1,2,3});
            require(std::get<std::vector<double>>(retained.graphicsHandle->property("Color")) == std::vector<double>{1,0,0});
            const auto oldGraph = session->graphicsGraph();
            session->reset();
            require(session->graphicsGraph() != oldGraph && session->graphicsGraph()->nodes().empty());
            require(retained.graphicsHandle->valid());
        }
        {
            auto registry = defaultBuiltinRegistry();
            std::vector<RuntimeValue> arguments{makeRuntimeVectorValue({1,2})};
            const auto missing = registry->invoke("plot", BuiltinCall{arguments, 1, {}});
            require(!missing.succeeded);
            BuiltinCallContext context;
            context.graphicsGraph = std::make_shared<RuntimeGraphicsGraph>();
            arguments.push_back(makeRuntimeVectorValue({1}));
            require(!registry->invoke("plot", BuiltinCall{arguments, 1, {}, &context}).succeeded);
            require(context.graphicsGraph->nodes().empty());
        }
        auto graph = std::make_shared<RuntimeGraphicsGraph>();
        {
            auto ownedGraph = std::make_shared<RuntimeGraphicsGraph>();
            std::weak_ptr<RuntimeGraphicsGraph> released = ownedGraph;
            const auto id = ownedGraph->plot({1, 2});
            auto value = makeRuntimeGraphicsValue(RuntimeGraphicsHandle(ownedGraph, id));
            auto alias = value;
            auto independent = makeRuntimeGraphicsValue(RuntimeGraphicsHandle(ownedGraph, id));
            require(runtimeValuesEqual(value, alias) && runtimeValuesEqual(value, independent));
            auto foreign = std::make_shared<RuntimeGraphicsGraph>();
            auto other = makeRuntimeGraphicsValue(RuntimeGraphicsHandle(foreign, foreign->plot({1, 2})));
            require(!runtimeValuesEqual(value, other));
            require(value.className == "matlab.graphics.chart.primitive.Line");
            require(value.sharedFields->empty());
            require(validateRuntimeValueContract(value).valid);
            auto malformed = value;
            malformed.kind = RuntimeValueKind::Number;
            require(!validateRuntimeValueContract(malformed).valid);
            malformed = value;
            malformed.className = "unrelated.Class";
            require(!validateRuntimeValueContract(malformed).valid);
            malformed = {};
            ownedGraph.reset();
            require(!released.expired());
            alias.graphicsHandle->setProperty("DisplayName", std::string{"shared"});
            require(std::get<std::string>(independent.graphicsHandle->property("DisplayName")) == "shared");
            value.graphicsHandle->erase();
            const auto deleted = nlohmann::json::parse(value.graphicsHandle->serialize());
            require(deleted.at("valid") == false && deleted.at("reference").is_null());
            require(!alias.graphicsHandle->valid() && !independent.graphicsHandle->valid());
            require(runtimeValuesEqual(value, independent));
            require(validateRuntimeValueContract(value).valid);
            value = {};
            alias = {};
            independent = {};
            require(released.expired());
        }
        {
            auto plotGraph = std::make_shared<RuntimeGraphicsGraph>();
            RuntimeGraphicsHandle plotted(plotGraph, plotGraph->plot({3, 5, 8}));
            require(std::get<std::vector<double>>(plotted.property("XData")) == std::vector<double>{1, 2, 3});
            require(std::get<std::vector<double>>(plotted.property("YData")) == std::vector<double>{3, 5, 8});
            require(plotGraph->nodes().size() == 3 && !plotted.parent()->parent()->parent());
            require(std::get<std::vector<double>>(plotted.parent()->property("YLim")) == std::vector<double>{3, 8});
            RuntimeGraphicsGraph equivalent;
            const auto discarded = equivalent.create(RuntimeGraphicsKind::Figure);
            equivalent.erase(discarded);
            (void)equivalent.plot({3, 5, 8});
            require(equivalent.serialize() == plotGraph->serialize());
            const auto record = nlohmann::json::parse(plotGraph->serialize());
            require(record.at("schema") == "mparser.graphics" && record.at("version") == 1);
            const auto& objects = record.at("objects");
            require(objects.size() == 3 && objects[0].at("type") == "Figure" &&
                objects[1].at("type") == "Axes" && objects[2].at("type") == "Line");
            require(objects[0].at("parent").is_null() && objects[1].at("parent") == 1 &&
                objects[2].at("parent") == 2);
            require(objects[0].at("children") == nlohmann::json::array({2}) &&
                objects[1].at("children") == nlohmann::json::array({3}));
            require(objects[2].at("properties").at("XData") == nlohmann::json::array({1, 2, 3}));
            bool badPlot = false;
            try { (void)plotGraph->plot({1, 2}, {3}); }
            catch (const std::invalid_argument&) { badPlot = true; }
            require(badPlot && plotGraph->nodes().size() == 3);
            RuntimeGraphicsHandle explicitPlot(plotGraph, plotGraph->plot({10, 20}, {2, 4}));
            require(explicitPlot.id() == 6 && plotGraph->nodes().size() == 6);
            require(std::get<std::vector<double>>(explicitPlot.property("XData")) == std::vector<double>{10, 20});
            plotted.parent()->parent()->erase();
            require(!plotted.valid() && explicitPlot.valid());
            require(plotGraph->serialize().find("\"id\":4") == std::string::npos);
            RuntimeGraphicsHandle constant(plotGraph, plotGraph->plot({5, 5}));
            const auto limits = std::get<std::vector<double>>(constant.parent()->property("YLim"));
            require(limits[0] < 5 && limits[1] > 5);
            RuntimeGraphicsHandle empty(plotGraph, plotGraph->plot(std::vector<double>{}));
            require(std::get<std::vector<double>>(empty.property("XData")).empty());
            require(std::get<std::vector<double>>(empty.parent()->property("XLim")) == std::vector<double>{0, 1});
            RuntimeGraphicsHandle exceptional(plotGraph, plotGraph->plot({
                std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                -std::numeric_limits<double>::infinity(), -0.0}));
            exceptional.setProperty("DisplayName", std::string{"quoted \"line\"\n"});
            const auto encoded = nlohmann::json::parse(plotGraph->serialize());
            const auto& properties = encoded.at("objects").back().at("properties");
            require(properties.at("YData") == nlohmann::json::array({"NaN", "Inf", "-Inf", 0.0}));
            require(properties.at("DisplayName") == "quoted \"line\"\n");
            exceptional.setProperty("DisplayName", std::string(1, static_cast<char>(0xff)));
            bool invalidText = false;
            try { (void)plotGraph->serialize(); } catch (const nlohmann::json::exception&) { invalidText = true; }
            require(invalidText);
        }
        std::weak_ptr<RuntimeGraphicsGraph> lifetime = graph;
        const auto figure = graph->create(RuntimeGraphicsKind::Figure);
        const auto axes = graph->create(RuntimeGraphicsKind::Axes, figure);
        const auto line = graph->create(RuntimeGraphicsKind::Line, axes);
        const auto other = graph->create(RuntimeGraphicsKind::Axes, figure);
        {
            RuntimeGraphicsHandle handle(graph, line);
            auto alias = handle;
            require(handle.sameIdentity(alias));
            handle.setProperty("XData", std::vector<double>{1, 2, 3});
            alias.setProperty("YData", std::vector<double>{4, 5, 6});
            require(std::get<std::vector<double>>(handle.property("YData"))[1] == 5);
            alias.setProperty("DisplayName", std::string{"series"});
            require(std::get<std::string>(handle.property("DisplayName")) == "series");
            const auto color = handle.property("Color");
            for (const auto& invalidColor : {std::vector<double>{1, 2, 0}, std::vector<double>{0, 1},
                    std::vector<double>{0, std::numeric_limits<double>::quiet_NaN(), 1}}) {
                bool deniedColor = false;
                try { alias.setProperty("Color", invalidColor); }
                catch (const std::invalid_argument&) { deniedColor = true; }
                require(deniedColor && handle.property("Color") == color);
            }
            RuntimeGraphicsHandle axesHandle(graph, axes);
            bool badLimits = false;
            try { axesHandle.setProperty("XLim", std::vector<double>{3, 1}); }
            catch (const std::invalid_argument&) { badLimits = true; }
            require(badLimits && std::get<std::vector<double>>(axesHandle.property("XLim"))[0] == 0);
            for (const auto& name : {"Unknown", "Parent", "Color"}) {
                bool badProperty = false;
                try { handle.setProperty(name, std::string{"invalid"}); }
                catch (const std::invalid_argument&) { badProperty = true; }
                require(badProperty);
            }
            auto foreignGraph = std::make_shared<RuntimeGraphicsGraph>();
            const auto foreignFigure = foreignGraph->create(RuntimeGraphicsKind::Figure);
            const auto foreignAxes = foreignGraph->create(RuntimeGraphicsKind::Axes, foreignFigure);
            RuntimeGraphicsHandle foreign(foreignGraph, foreignAxes);
            bool denied = false;
            try { handle.reparent(foreign); } catch (const std::invalid_argument&) { denied = true; }
            require(denied && handle.parent()->id() == axes);
            require(!RuntimeGraphicsHandle(graph, axes).sameIdentity(foreign));
            require(handle.parent()->parent()->children().size() == 2);
        }
        graph->reparent(line, other);
        require(graph->find(axes)->children.empty());
        require(graph->find(other)->children == std::vector<RuntimeGraphicsId>{line});
        require(graph->find(line)->parent == other);
        bool rejected = false;
        try { graph->reparent(figure, line); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && !graph->find(figure)->parent);
        rejected = false;
        try { graph->create(RuntimeGraphicsKind::Line, figure); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && graph->nodes().size() == 4);
        graph->erase(other);
        require(!graph->find(other) && !graph->find(line));
        const auto replacement = graph->create(RuntimeGraphicsKind::Axes, figure);
        require(replacement > other);
        graph->erase(figure);
        require(graph->nodes().empty());
        {
            const auto retainedId = graph->create(RuntimeGraphicsKind::Figure);
            RuntimeGraphicsHandle retained(graph, retainedId);
            auto alias = retained;
            graph.reset();
            require(!lifetime.expired() && retained.valid());
            retained.erase();
            require(!alias.valid() && retained.sameIdentity(alias));
            bool denied = false;
            try { (void)alias.children(); } catch (const std::invalid_argument&) { denied = true; }
            require(denied);
            denied = false;
            try { alias.setProperty("Name", std::string{"deleted"}); }
            catch (const std::invalid_argument&) { denied = true; }
            require(denied);
        }
        require(lifetime.expired());
        std::cout << "graphics graph = identity,parent,children,reparent,delete,lifetime\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
