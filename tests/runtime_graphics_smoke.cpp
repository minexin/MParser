#include "mparser/runtime/core/object_model/runtime_graphics.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_text.h"
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
        require(argc == 4);
        std::ifstream identitySchemaInput(argv[3]);
        nlohmann::json identitySchema;
        identitySchemaInput >> identitySchema;
        nlohmann::json_schema::json_validator identityValidator;
        identityValidator.set_root_schema(identitySchema);
        {
            RuntimeGraphicsGraph graph;
            const auto first = graph.currentFigure();
            const auto firstAxes = graph.currentAxes();
            require(graph.currentFigure() == first && graph.currentAxes() == firstAxes);
            const auto second = graph.create(RuntimeGraphicsKind::Figure);
            const auto secondAxes = graph.currentAxes();
            require(second != first && secondAxes != firstAxes);
            graph.selectFigure(first);
            require(graph.currentAxes() == firstAxes);
            graph.selectAxes(secondAxes);
            require(graph.currentFigure() == second);
            graph.reparent(secondAxes, first);
            graph.selectFigure(first);
            require(graph.currentAxes() == secondAxes);
            graph.erase(secondAxes);
            require(graph.currentAxes() == firstAxes);
            graph.erase(first);
            require(graph.currentFigure() == second);
            require(graph.find(firstAxes) == nullptr);
            graph.erase(second);
            const auto replacement = graph.currentFigure();
            require(replacement != first && replacement != second);
            bool rejected = false;
            try { graph.selectFigure(first); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected && graph.currentFigure() == replacement);
        }
        std::ifstream schemaInput(argv[1]);
        {
            auto graph = std::make_shared<RuntimeGraphicsGraph>();
            const auto first = graph->plot({1, 2});
            graph->setProperty(graph->currentAxes(), "NextPlot", std::string{"add"});
            const auto survivor = graph->plot({3, 4});
            const auto before = nlohmann::json::parse(graph->serialize(true));
            graph->erase(first);
            const auto after = nlohmann::json::parse(graph->serialize(true));
            identityValidator.validate(before);
            identityValidator.validate(after);
            require(before.at("graphIdentity") == after.at("graphIdentity"));
            require(before.at("objects").back().at("id") != after.at("objects").back().at("id"));
            require(before.at("objects").back().at("objectIdentity") ==
                after.at("objects").back().at("objectIdentity"));
            require(after.at("objects").back().at("objectIdentity") == std::to_string(survivor));
            graph->erase(graph->currentFigure());
            const auto empty = nlohmann::json::parse(graph->serialize(true));
            identityValidator.validate(empty);
            require(empty.at("objects").empty() && empty.at("graphIdentity") == before.at("graphIdentity"));
        }
        {
            auto graph = std::make_shared<RuntimeGraphicsGraph>();
            RuntimeGraphicsHandle original(graph, graph->plot({1, 2, 3}));
            RuntimeGraphicsHandle alias = original;
            const auto initial = nlohmann::json::parse(original.serialize());
            const auto identityBefore = nlohmann::json::parse(graph->serialize(true));
            identityValidator.validate(identityBefore);
            auto expectInvalidIdentity = [&](nlohmann::json invalid) {
                bool rejected = false;
                try { identityValidator.validate(invalid); }
                catch (const std::exception&) { rejected = true; }
                require(rejected);
            };
            auto malformed = identityBefore;
            malformed.erase("graphIdentity");
            expectInvalidIdentity(malformed);
            malformed = identityBefore;
            malformed["objects"][0].erase("objectIdentity");
            expectInvalidIdentity(malformed);
            for (const auto& invalid : {nlohmann::json(3), nlohmann::json("0"),
                nlohmann::json("01"), nlohmann::json("-1"), nlohmann::json("1.5")}) {
                malformed = identityBefore;
                malformed["graphIdentity"] = invalid;
                expectInvalidIdentity(malformed);
                malformed = identityBefore;
                malformed["objects"][0]["objectIdentity"] = invalid;
                expectInvalidIdentity(malformed);
            }
            expectInvalidIdentity(nlohmann::json::parse(graph->serialize()));
            require(identityBefore.at("version") == 3);
            const auto originalIdentity = identityBefore.at("objects").back().at("objectIdentity");
            original.setProperty("DisplayName", std::string{"updated"});
            require(original.sameIdentity(alias));
            require(nlohmann::json::parse(graph->serialize(true)).at("objects").back().at("objectIdentity") ==
                originalIdentity);
            graph->erase(graph->currentFigure());
            RuntimeGraphicsHandle replacement(graph, graph->plot({1, 2, 3}));
            require(!original.valid() && !alias.valid() && replacement.valid());
            require(!original.sameIdentity(replacement));
            const auto before = graph->serialize();
            const auto identityAfter = nlohmann::json::parse(graph->serialize(true));
            require(identityAfter.at("graphIdentity") == identityBefore.at("graphIdentity"));
            require(identityAfter.at("objects").back().at("objectIdentity") != originalIdentity);
            require(!nlohmann::json::parse(before).contains("graphIdentity"));
            bool rejected = false;
            try { alias.setProperty("DisplayName", std::string{"stale write"}); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected && graph->serialize() == before);
            const auto stale = nlohmann::json::parse(original.serialize());
            require(stale.at("valid") == false && stale.at("reference").is_null());
            // Legacy references are local to a snapshot, not persistent identities.
            require(initial.at("reference") ==
                nlohmann::json::parse(replacement.serialize()).at("reference"));
            auto independent = std::make_shared<RuntimeGraphicsGraph>();
            RuntimeGraphicsHandle other(independent, independent->plot({1, 2, 3}));
            require(original.id() == other.id() && !original.sameIdentity(other));
            require(nlohmann::json::parse(independent->serialize(true)).at("graphIdentity") !=
                identityBefore.at("graphIdentity"));
        }
        {
            auto graph = std::make_shared<RuntimeGraphicsGraph>();
            RuntimeGraphicsHandle first(graph, graph->plot({1, 2}));
            const auto axes = graph->currentAxes();
            const auto figure = graph->currentFigure();
            graph->setProperty(axes, "NextPlot", std::string{"add"});
            RuntimeGraphicsHandle second(graph, graph->plot({10, 20}, {-5, 8}));
            require(first.valid() && second.valid());
            require(graph->nodes().size() == 4 && graph->currentFigure() == figure);
            require(std::get<std::vector<double>>(graph->find(axes)->properties.at("XLim")) ==
                std::vector<double>{1, 20});
            require(std::get<std::vector<double>>(graph->find(axes)->properties.at("YLim")) ==
                std::vector<double>{-5, 8});
            const auto before = graph->serialize();
            bool rejected = false;
            try { graph->plot({1, 2}, {3}); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected && graph->serialize() == before && graph->currentAxes() == axes);
            rejected = false;
            try { graph->plotLines({{{1, 2}, {3, 4}}, {{1, 2}, {3}}}); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected && graph->serialize() == before && first.valid() && second.valid());
            rejected = false;
            try { graph->setProperty(axes, "NextPlot", std::string{"invalid"}); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected && graph->serialize() == before);
            graph->setProperty(axes, "NextPlot", std::string{"replace"});
            RuntimeGraphicsHandle replacement(graph, graph->plot({4, 5}));
            require(replacement.id() == second.id() + 1 && !first.valid() && !second.valid());
            require(graph->nodes().size() == 3 && graph->currentAxes() == axes);
        }
        nlohmann::json schema;
        schemaInput >> schema;
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(schema);
        std::ifstream historicalSchemaInput(argv[2]);
        nlohmann::json historicalSchema;
        historicalSchemaInput >> historicalSchema;
        nlohmann::json_schema::json_validator historicalValidator;
        historicalValidator.set_root_schema(historicalSchema);
        {
            auto graph = std::make_shared<RuntimeGraphicsGraph>();
            const auto figure = graph->currentFigure();
            const auto axes = graph->currentAxes();
            bool invalidParent = false;
            try { graph->create(RuntimeGraphicsKind::Legend, axes); }
            catch (const std::invalid_argument&) { invalidParent = true; }
            require(invalidParent && graph->nodes().size() == 2);
            const auto named = graph->create(RuntimeGraphicsKind::Line, axes);
            graph->setProperty(named, "DisplayName", std::string{"measured"});
            const auto unnamed = graph->create(RuntimeGraphicsKind::Line, axes);
            const auto id = graph->legend(axes);
            auto value = makeRuntimeGraphicsValue(RuntimeGraphicsHandle(graph, id));
            require(isRuntimeGraphicsValue(value) && validateRuntimeValueContract(value).valid);
            require(value.className == "matlab.graphics.illustration.Legend");
            require(std::get<std::string>(value.graphicsHandle->property("Location")) == "northeast");
            require(runtimeSetGraphicsMember(value, "Visible", makeRuntimeNumberValue(0)).succeeded);
            require(runtimeSetGraphicsMember(value, "Location", makeRuntimeCharacterVectorUtf8("southoutside")).succeeded);
            require(runtimeSetGraphicsMember(value, "Orientation", makeRuntimeCharacterVectorUtf8("horizontal")).succeeded);
            require(graph->legend(axes) == id);
            require(std::get<std::string>(value.graphicsHandle->property("Location")) == "southoutside");
            const auto before = graph->serialize();
            require(!runtimeSetGraphicsMember(value, "Location", makeRuntimeCharacterVectorUtf8("invalid")).succeeded);
            require(!runtimeSetGraphicsMember(value, "Orientation", makeRuntimeCharacterVectorUtf8("diagonal")).succeeded);
            require(!runtimeSetGraphicsMember(value, "Visible", makeRuntimeNumberValue(2)).succeeded);
            require(graph->serialize() == before);
            ModuleInvocationResult result;
            result.outputs.push_back(value);
            auto wire = nlohmann::json::parse(serializeMachineResultJsonV1(result, "legend"));
            validator.validate(wire);
            auto& encoded = wire["outputs"][0]["value"]["graphics"]["graph"];
            require(encoded.at("version") == 2 && encoded.at("objects").back().at("type") == "Legend");
            require(encoded.at("objects").back().at("axes") == encoded.at("objects")[1].at("id"));
            const auto& entries = encoded.at("objects").back().at("entries");
            require(entries.size() == 2 && entries[0].at("label") == "measured" && entries[1].at("label") == "data2");
            require(entries[0].at("line") == encoded.at("objects")[2].at("id") &&
                entries[1].at("line") == encoded.at("objects")[3].at("id"));
            graph->erase(unnamed);
            graph->setProperty(named, "DisplayName", std::string{"renamed"});
            const auto refreshed = nlohmann::json::parse(graph->serialize()).at("objects").back().at("entries");
            require(refreshed.size() == 1 && refreshed[0].at("label") == "renamed");
            require(encoded.at("objects").back().at("properties").at("Visible") == "off");
            encoded["version"] = 1;
            bool rejected = false;
            try { validator.validate(wire); }
            catch (const std::exception&) { rejected = true; }
            require(rejected);
            const auto destination = graph->create(RuntimeGraphicsKind::Figure);
            graph->reparent(axes, destination);
            require(value.graphicsHandle->parent()->id() == destination);
            require(graph->find(figure)->children.empty());
            bool wrongParent = false;
            try { graph->reparent(id, figure); }
            catch (const std::invalid_argument&) { wrongParent = true; }
            require(wrongParent && value.graphicsHandle->parent()->id() == destination);
            const auto secondAxes = graph->create(RuntimeGraphicsKind::Axes, destination);
            const auto secondLegend = graph->legend(secondAxes);
            require(secondLegend != id);
            graph->erase(axes);
            require(!value.graphicsHandle->valid() && graph->find(secondLegend));
            require(graph->find(destination)->children.size() == 2);
            (void)graph->serialize();
            graph->erase(secondLegend);
            const auto replacement = graph->legend(secondAxes);
            require(replacement != secondLegend);
            graph->erase(destination);
            require(!graph->find(replacement) && graph->nodes().size() == 1);
            graph->erase(figure);
            require(graph->nodes().empty());
        }
        {
            auto textGraph = std::make_shared<RuntimeGraphicsGraph>();
            const auto axes = textGraph->currentAxes();
            const auto id = textGraph->create(RuntimeGraphicsKind::Text, axes);
            auto value = makeRuntimeGraphicsValue(RuntimeGraphicsHandle(textGraph, id));
            require(value.className == "matlab.graphics.primitive.Text");
            require(isRuntimeGraphicsValue(value) && validateRuntimeValueContract(value).valid);
            require(runtimeSetGraphicsMember(value, "String", makeRuntimeCharacterVectorUtf8("Measured gain")).succeeded);
            require(runtimeSetGraphicsMember(value, "Color", makeRuntimeVectorValue({1, 0, 0})).succeeded);
            ModuleInvocationResult result;
            result.outputs.push_back(value);
            auto wire = nlohmann::json::parse(serializeMachineResultJsonV1(result, "text"));
            validator.validate(wire);
            auto& graph = wire["outputs"][0]["value"]["graphics"]["graph"];
            require(graph.at("version") == 2);
            require(graph.at("objects").back().at("type") == "Text");
            require(graph.at("objects").back().at("properties").at("String") == "Measured gain");
            graph["version"] = 1;
            bool rejected = false;
            try { validator.validate(wire); }
            catch (const std::exception&) { rejected = true; }
            require(rejected);
            graph["version"] = 3;
            rejected = false;
            try { validator.validate(wire); }
            catch (const std::exception&) { rejected = true; }
            require(rejected);
            const auto otherAxes = textGraph->create(RuntimeGraphicsKind::Axes, textGraph->currentFigure());
            textGraph->reparent(id, otherAxes);
            require(value.graphicsHandle->parent()->id() == otherAxes);
            textGraph->erase(otherAxes);
            require(!value.graphicsHandle->valid());
            const auto title = textGraph->setLabel(axes, "Title", "first");
            require(textGraph->setLabel(axes, "Title", "second") == title);
            RuntimeGraphicsHandle titleHandle(textGraph, title);
            require(std::get<std::string>(titleHandle.property("String")) == "second");
            titleHandle.setProperty("String", std::string{"edited"});
            auto labelGraph = nlohmann::json::parse(textGraph->serialize());
            require(labelGraph.at("version") == 2);
            const auto& axesRecord = labelGraph.at("objects")[1];
            require(axesRecord.at("labels").contains("Title"));
            require(!axesRecord.at("properties").contains("Title"));
            require(labelGraph.at("objects").back().at("properties").at("String") == "edited");
            ModuleInvocationResult labelResult;
            labelResult.outputs.push_back(makeRuntimeGraphicsValue(titleHandle));
            validator.validate(nlohmann::json::parse(serializeMachineResultJsonV1(labelResult, "label")));
            const auto destination = textGraph->create(RuntimeGraphicsKind::Axes, textGraph->currentFigure());
            textGraph->reparent(title, destination);
            require(textGraph->find(axes)->labels.empty());
            const auto replacement = textGraph->setLabel(axes, "Title", "replacement");
            require(replacement != title);
            textGraph->erase(replacement);
            require(textGraph->find(axes)->labels.empty());
            const auto before = textGraph->serialize();
            auto axesValue = makeRuntimeGraphicsValue(RuntimeGraphicsHandle(textGraph, axes));
            require(RuntimeGraphicsHandle(textGraph, axes).children().empty());
            require(!runtimeSetGraphicsMember(axesValue, "Title", makeRuntimeGraphicsValue(titleHandle)).succeeded);
            require(textGraph->serialize() == before && textGraph->find(axes)->labels.empty());
            bool invalidRole = false;
            try { textGraph->setLabel(axes, "Unknown", "bad"); }
            catch (const std::invalid_argument&) { invalidRole = true; }
            require(invalidRole && textGraph->serialize() == before);
            const auto bound = textGraph->setLabel(axes, "XLabel", "time");
            RuntimeGraphicsHandle boundHandle(textGraph, bound);
            require(RuntimeGraphicsHandle(textGraph, axes).children().empty());
            require(textGraph->find(axes)->children.size() == 1);
            textGraph->erase(axes);
            require(!boundHandle.valid());
        }
        for (bool hir : {false, true}) {
            auto labelsModule = CompiledModule::compile(
                "line=plot([1,2]); ax=gca(); initial=ax.Title; emptyLabel=isempty(initial.String);"
                "t=title('first'); u=title('second'); same=t==u; lazySame=initial==t;"
                "updated=strcmp(t.String,'second'); t.String='edited'; edited=strcmp(u.String,'edited');"
                "x=xlabel('time'); y=ylabel(ax,'gain'); xok=strcmp(x.String,'time'); yok=strcmp(y.String,'gain');"
                "parentOk=t.Parent==ax; children=ax.Children; childrenOk=isequal(children,line);"
                "bindingOk=ax.Title==t; alias=ax.Title; alias.String='alias';"
                "aliasOk=strcmp(t.String,'alias'); ax.Title.String='nested'; nestedOk=strcmp(t.String,'nested');"
                "ax.Title.Color=[1,0,0]; colorOk=isequal(t.Color,[1,0,0]);"
                "badNested=false; try; ax.Title.Color=[2,0,0]; catch; badNested=true; end;"
                "colorKept=isequal(t.Color,[1,0,0]);"
                "other=axes(); v=title(ax,'target'); targetOk=v==t;"
                "currentOk=gca()==other; bad=false; try; title(ax,42); catch; bad=true; end;"
                "badAssignment=false; try; ax.Title='legacy'; catch; badAssignment=true; end;"
                "preserved=strcmp(t.String,'target'); close all; gone=~isvalid(t);");
            require(labelsModule.valid());
            std::vector<RuntimeVariable> labelVariables;
            if (hir) {
                auto result = Interpreter{}.run(labelsModule.semantic());
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.diagnostics.empty());
                labelVariables = std::move(result.variables);
            } else {
                auto result = labelsModule.execute();
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.succeeded());
                labelVariables = std::move(result.variables);
            }
            size_t labelChecks = 0;
            for (const auto& variable : labelVariables) {
                if (variable.name == "same" || variable.name == "updated" || variable.name == "edited" ||
                    variable.name == "xok" || variable.name == "yok" || variable.name == "parentOk" ||
                    variable.name == "targetOk" || variable.name == "currentOk" || variable.name == "bad" ||
                    variable.name == "preserved" || variable.name == "gone" ||
                    variable.name == "bindingOk" || variable.name == "aliasOk" || variable.name == "badAssignment" ||
                    variable.name == "emptyLabel" || variable.name == "lazySame" || variable.name == "nestedOk" ||
                    variable.name == "colorOk" || variable.name == "badNested" || variable.name == "colorKept" ||
                    variable.name == "childrenOk") {
                    require(runtimeNumericTruthValue(variable.value).value_or(false));
                    ++labelChecks;
                }
            }
            require(labelChecks == 21);
            auto gridModule = CompiledModule::compile(
                "ax=gca(); defaultOff=strcmp(ax.XGrid,'off')&&strcmp(ax.YGrid,'off');"
                "grid on; enabled=strcmp(ax.XGrid,'on')&&strcmp(ax.YGrid,'on');"
                "grid; toggled=strcmp(ax.XGrid,'off')&&strcmp(ax.YGrid,'off');"
                "other=axes(); grid(ax,true); explicitOn=strcmp(ax.YGrid,'on');"
                "otherOff=strcmp(other.YGrid,'off'); grid(ax,0); explicitOff=strcmp(ax.XGrid,'off');"
                "bad=false; try; grid(ax,2); catch; bad=true; end; unchanged=strcmp(ax.XGrid,'off');"
                "axes(ax); grid on; hold on; plot([1,2]); heldGrid=strcmp(ax.XGrid,'on');"
                "hold off; plot([3,4]); resetGrid=strcmp(ax.YGrid,'off');");
            require(gridModule.valid());
            std::vector<RuntimeVariable> gridVariables;
            if (hir) {
                auto result = Interpreter{}.run(gridModule.semantic());
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.diagnostics.empty());
                gridVariables = std::move(result.variables);
            } else {
                auto result = gridModule.execute();
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.succeeded());
                gridVariables = std::move(result.variables);
            }
            size_t gridChecks = 0;
            for (const auto& variable : gridVariables) {
                if (variable.name != "ax" && variable.name != "other") {
                    require(runtimeNumericTruthValue(variable.value).value_or(false));
                    ++gridChecks;
                }
            }
            require(gridChecks == 10);
            auto closing = CompiledModule::compile(
                "emptyStatus=close(); f1=figure(); h1=plot([1,2]); a1=h1.Parent;"
                "f2=figure(); h2=plot([3,4]); figure(f1); status=close();"
                "goneFigure=~isvalid(f1); goneAxes=~isvalid(a1); goneLine=~isvalid(h1);"
                "otherKept=isvalid(f2); fallback=gcf()==f2;"
                "rejected=false; try; close(h2); catch; rejected=true; end; kept=isvalid(h2);"
                "f3=figure(); close([f2,f3]); arrayGone=~isvalid(f2)&&~isvalid(f3);"
                "fresh=plot([8,9]); recreated=isvalid(fresh); close all; allGone=~isvalid(fresh); close;");
            require(closing.valid());
            std::vector<RuntimeVariable> closingVariables;
            if (hir) {
                auto result = Interpreter{}.run(closing.semantic());
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.diagnostics.empty());
                closingVariables = std::move(result.variables);
            } else {
                auto result = closing.execute();
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.succeeded());
                closingVariables = std::move(result.variables);
            }
            size_t closingChecks = 0;
            for (const auto& variable : closingVariables) {
                if (variable.name == "emptyStatus" || variable.name == "status" ||
                    variable.name == "goneFigure" || variable.name == "goneAxes" ||
                    variable.name == "goneLine" || variable.name == "otherKept" ||
                    variable.name == "fallback" || variable.name == "rejected" ||
                    variable.name == "kept" || variable.name == "arrayGone" ||
                    variable.name == "recreated" || variable.name == "allGone") {
                    require(runtimeNumericTruthValue(variable.value).value_or(false));
                    ++closingChecks;
                }
            }
            require(closingChecks == 12);
            auto clipping = CompiledModule::compile(
                "line=plot([-2,0,2]); ax=line.Parent;"
                "lineDefault=strcmp(line.Clipping,'on'); axesDefault=strcmp(ax.Clipping,'on');"
                "ax.XLim=[1,2]; ax.YLim=[-1,1]; line.Clipping='off';"
                "textOff=strcmp(line.Clipping,'off'); ax.Clipping=0;"
                "numericOff=strcmp(ax.Clipping,'off'); line.Clipping=true;"
                "logicalOn=strcmp(line.Clipping,'on');"
                "badNumber=false; try; line.Clipping=2; catch; badNumber=true; end;"
                "badText=false; try; ax.Clipping='invalid'; catch; badText=true; end;"
                "unchangedLine=strcmp(line.Clipping,'on'); unchangedAxes=strcmp(ax.Clipping,'off');"
                "samples=line.YData; hold on; plot([3,4]); held=strcmp(ax.Clipping,'off');"
                "hold off; plot([5,6]); reset=strcmp(ax.Clipping,'on');");
            require(clipping.valid());
            std::vector<RuntimeVariable> clippingVariables;
            if (hir) {
                auto result = Interpreter{}.run(clipping.semantic());
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.diagnostics.empty());
                clippingVariables = std::move(result.variables);
            } else {
                auto result = clipping.execute();
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.succeeded());
                clippingVariables = std::move(result.variables);
            }
            size_t clippingChecks = 0;
            for (const auto& variable : clippingVariables) {
                if (variable.name == "lineDefault" || variable.name == "axesDefault" ||
                    variable.name == "textOff" || variable.name == "numericOff" ||
                    variable.name == "logicalOn" || variable.name == "badNumber" ||
                    variable.name == "badText" || variable.name == "unchangedLine" ||
                    variable.name == "unchangedAxes" || variable.name == "held" || variable.name == "reset") {
                    require(runtimeNumericTruthValue(variable.value).value_or(false));
                    ++clippingChecks;
                }
                if (variable.name == "samples") {
                    require(variable.value.elements == std::vector<double>{-2, 0, 2});
                    ++clippingChecks;
                }
            }
            require(clippingChecks == 12);
            auto matrixPlot = CompiledModule::compile(
                "h=plot([1,10;2,20;3,30]); shape=size(h);"
                "y1=h(1).YData; y2=h(2).YData; x1=h(1).XData;"
                "ax=gca(); sameAxes=h(2).Parent==ax;"
                "r=plot([4,5,6],[1,2,3;10,20,30]); rowY=r(2).YData; rowX=r(2).XData;"
                "s=plot([7,8],[1,2;3,4]); squareY=s(2).YData;"
                "m=plot([1,2;3,4],[5,6;7,8]); matrixX=m(2).XData; matrixY=m(2).YData;"
                "v=plot([1,2,3;10,20,30],[4;5;6]); reversedX=v(2).XData; reversedY=v(2).YData;"
                "rejected=false; try; plot(1:4,[1,2,3;4,5,6]); catch; rejected=true; end;"
                "retained=isvalid(v(1));");
            require(matrixPlot.valid());
            std::vector<RuntimeVariable> matrixVariables;
            if (hir) {
                auto result = Interpreter{}.run(matrixPlot.semantic());
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.diagnostics.empty());
                matrixVariables = std::move(result.variables);
            } else {
                auto result = matrixPlot.execute();
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.succeeded());
                matrixVariables = std::move(result.variables);
            }
            const std::map<std::string, std::vector<double>> expectedMatrixValues{
                {"shape", {2, 1}}, {"y1", {1, 2, 3}}, {"y2", {10, 20, 30}}, {"x1", {1, 2, 3}},
                {"rowY", {10, 20, 30}}, {"rowX", {4, 5, 6}}, {"squareY", {2, 4}},
                {"matrixX", {2, 4}}, {"matrixY", {6, 8}},
                {"reversedX", {10, 20, 30}}, {"reversedY", {4, 5, 6}}};
            size_t matrixChecks = 0;
            for (const auto& variable : matrixVariables) {
                const auto found = expectedMatrixValues.find(variable.name);
                if (found != expectedMatrixValues.end()) {
                    require(variable.value.elements == found->second);
                    ++matrixChecks;
                }
                if (variable.name == "sameAxes" || variable.name == "rejected" || variable.name == "retained") {
                    require(runtimeNumericTruthValue(variable.value).value_or(false));
                }
            }
            require(matrixChecks == expectedMatrixValues.size());
            auto replacement = CompiledModule::compile(
                "f=figure(); ax=axes(); first=plot([1,2]);"
                "second=plot([3,4]); stale=~isvalid(first);"
                "sameAxes=second.Parent==ax; sameFigure=gcf()==f;"
                "hold(ax,'on'); third=plot([10,20]); kept=isvalid(second);"
                "hold(ax); toggled=strcmp(ax.NextPlot,'replace'); hold on;"
                "label=title(ax,'preserved'); ax.NextPlot='replacechildren'; fourth=plot([5,6]);"
                "removed=~isvalid(third); titleKept=strcmp(ax.Title.String,'preserved');"
                "hold off; fifth=plot([7,8]); titleReset=isempty(ax.Title.String); labelIdentity=ax.Title==label;");
            require(replacement.valid());
            std::vector<RuntimeVariable> replacementVariables;
            if (hir) {
                auto result = Interpreter{}.run(replacement.semantic());
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.diagnostics.empty());
                replacementVariables = std::move(result.variables);
            } else {
                auto result = replacement.execute();
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.succeeded());
                replacementVariables = std::move(result.variables);
            }
            size_t replacementChecks = 0;
            for (const auto& variable : replacementVariables) {
                if (variable.name == "stale" || variable.name == "sameAxes" ||
                    variable.name == "sameFigure" || variable.name == "kept" ||
                    variable.name == "removed" || variable.name == "titleKept" ||
                    variable.name == "titleReset" || variable.name == "toggled" || variable.name == "labelIdentity") {
                    require(runtimeNumericTruthValue(variable.value).value_or(false));
                    ++replacementChecks;
                }
            }
            require(replacementChecks == 9);
            auto lifecycle = CompiledModule::compile(
                "f1=figure(); a1=gca(); f2=figure(); a2=axes();"
                "figure(f1); selected=gca(); same=selected==a1;"
                "axes(a2); selectedFigure=gcf(); sameFigure=selectedFigure==f2;"
                "different=f1~=f2;");
            require(lifecycle.valid());
            std::vector<RuntimeVariable> lifecycleVariables;
            if (hir) {
                auto result = Interpreter{}.run(lifecycle.semantic());
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.diagnostics.empty());
                lifecycleVariables = std::move(result.variables);
            } else {
                auto result = lifecycle.execute();
                for (const auto& diagnostic : result.diagnostics) { std::cerr << diagnostic.message << '\n'; }
                require(result.succeeded());
                lifecycleVariables = std::move(result.variables);
            }
            size_t checked = 0;
            for (const auto& variable : lifecycleVariables) {
                if (variable.name == "same" || variable.name == "sameFigure" || variable.name == "different") {
                    require(runtimeNumericTruthValue(variable.value).value_or(false));
                    ++checked;
                }
            }
            require(checked == 3);
            auto module = CompiledModule::compile(
                "a=plot(1:3); figure(); b=plot(3:-1:1); items=[a,b;a,b]; ax=a.Parent;"
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
                "h=plot([3,5,8]); alias=h; figure(); k=plot([2;4],[7;9]); figure(); plot([]); same=isequal(h,alias); different=isequal(h,k);"
                "ax=h.Parent; fig=ax.Parent; children=ax.Children; sameChild=isequal(children,h);"
                "alias.Color=[1,0,0]; color=h.Color; fig.Name='headless'; name=fig.Name;"
                "otherAx=k.Parent; h.Parent=otherAx; moved=isequal(h.Parent,otherAx);"
                "figure(); doomed=plot([4,5]); doomedAlias=doomed; doomedAx=doomed.Parent;"
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
            std::vector<RuntimeValue> noArguments;
            require(registry->invoke("close", BuiltinCall{noArguments, 1, {}, &context}).succeeded);
            require(context.graphicsGraph->nodes().empty());
            const auto figure = context.graphicsGraph->create(RuntimeGraphicsKind::Figure);
            auto foreign = std::make_shared<RuntimeGraphicsGraph>();
            const auto foreignFigure = foreign->create(RuntimeGraphicsKind::Figure);
            auto array = runtimeMakeObjectArrayFromLogicalOrder({
                makeRuntimeGraphicsValue(RuntimeGraphicsHandle(context.graphicsGraph, figure)),
                makeRuntimeGraphicsValue(RuntimeGraphicsHandle(foreign, foreignFigure))},
                {2, 1}, "matlab.ui.Figure", true);
            require(array.succeeded);
            std::vector<RuntimeValue> closeArguments{array.value};
            require(!registry->invoke("close", BuiltinCall{closeArguments, 0, {}, &context}).succeeded);
            require(context.graphicsGraph->find(figure) && foreign->find(foreignFigure));
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
            plotGraph->create(RuntimeGraphicsKind::Figure);
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
            alias.setProperty("Color", std::string{"r"});
            require(std::get<std::vector<double>>(handle.property("Color")) == std::vector<double>{1,0,0});
            alias.setProperty("Color", std::vector<double>{1,0,0,0.5});
            require(std::get<std::vector<double>>(handle.property("Color")) == std::vector<double>{1,0,0});
            const auto rgba = graph->serialize(true);
            bool invalidAlpha = false;
            try { alias.setProperty("Color", std::vector<double>{1,0,0,2}); }
            catch (const std::invalid_argument&) { invalidAlpha = true; }
            require(invalidAlpha && graph->serialize(true) == rgba);
            alias.setProperty("Color", std::string{"none"});
            require(std::get<std::string>(handle.property("Color")) == "none");
            alias.setProperty("Color", std::string{"#00f"});
            require(std::get<std::vector<double>>(handle.property("Color")) == std::vector<double>{0,0,1});
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
