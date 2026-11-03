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
        {
            const std::pair<const char*, const char*> cases[] = {
                {"print(gcf(),'unused.pdf','-dpdf');", "MParser:GraphicsRendererUnavailable"},
                {"saveas(gcf(),'unused.png');", "MParser:GraphicsRendererUnavailable"},
                {"saveas(gca(),'unused.pdf');", "MParser:InvalidExportFigure"},
                {"f=figure(); delete(f); saveas(f,'unused.pdf');", "MParser:InvalidExportFigure"},
                {"saveas(gcf(),'','pdf');", "MParser:InvalidGraphicsExport"},
                {"print(gcf(),'unused.pdf','-dunknown');", "MParser:UnsupportedExportFormat"},
                {"saveas(gcf(),'unused.xyz');", "MParser:UnsupportedExportFormat"}
            };
            for (const auto& [source, identifier] : cases) {
                const auto result = Module::compile(source, "export_boundary.m").execute();
                require(!result.succeeded());
                const auto diagnostics = result.diagnostics();
                require(!diagnostics.empty() && diagnostics.front().identifier == identifier);
            }
        }
        std::string firstSnapshot;
        std::string firstIdentitySnapshot;
        {
            auto runtime = Runtime::create();
            auto module = Module::compile("plot(1:3);", "no_output.m");
            firstPlot = runtime.execute(module);
            require(firstPlot.succeeded() && firstPlot.outputCount() == 0);
            firstSnapshot = firstPlot.graphicsJson();
            firstIdentitySnapshot = firstPlot.graphicsJson(3);
            const auto identity = nlohmann::json::parse(firstIdentitySnapshot);
            require(identity.at("version") == 3);
            require(firstPlot.graphicsJson(0) == firstSnapshot);
            require(nlohmann::json::parse(firstSnapshot).at("objects").size() == 3);
            auto second = runtime.execute(Module::compile("plot([8,9]);", "replacement.m"));
            require(second.succeeded());
            const auto replacement = nlohmann::json::parse(second.graphicsJson());
            require(replacement.at("objects").size() == 3);
            const auto replacementIdentity = nlohmann::json::parse(second.graphicsJson(3));
            require(replacementIdentity.at("graphIdentity") == identity.at("graphIdentity"));
            require(replacementIdentity.at("objects").back().at("objectIdentity") !=
                identity.at("objects").back().at("objectIdentity"));
            require(firstPlot.graphicsJson(3) == firstIdentitySnapshot);
            require(replacement.at("objects").back().at("properties").at("YData") ==
                nlohmann::json::array({8, 9}));
            require(firstPlot.graphicsJson() == firstSnapshot);
            auto closed = runtime.execute(Module::compile("close all;", "close.m"));
            require(closed.succeeded());
            const auto closedIdentity = nlohmann::json::parse(closed.graphicsJson(3));
            require(closedIdentity.at("objects").empty());
            require(closedIdentity.at("graphIdentity") == identity.at("graphIdentity"));
            require(nlohmann::json::parse(closed.graphicsJson()).at("objects").empty());
            require(firstPlot.graphicsJson() == firstSnapshot);
            runtime.reset();
            auto empty = runtime.execute(Module::compile("x=1;", "empty.m"));
            require(empty.succeeded());
            require(nlohmann::json::parse(empty.graphicsJson(3)).at("graphIdentity") !=
                identity.at("graphIdentity"));
            require(firstPlot.graphicsJson(3) == firstIdentitySnapshot);
            require(nlohmann::json::parse(empty.graphicsJson()).at("objects").empty());
            require(firstPlot.graphicsJson() == firstSnapshot);
        }
        require(firstPlot.graphicsJson() == firstSnapshot);
        require(firstPlot.graphicsJson(3) == firstIdentitySnapshot);
        auto failed = Module::compile("plot(1:3); error('after plot');", "failure.m").execute();
        require(!failed.succeeded());
        require(nlohmann::json::parse(failed.graphicsJson()).at("objects").size() == 3);
        auto invalid = Module::compile("function out = broken(\n", "invalid.m").execute();
        require(!invalid.succeeded() && invalid.graphicsJson().empty());
        require(invalid.graphicsJson(3).empty());
        require(mparser_result_graphics_json(nullptr).size == 0);
        mparser_module* cModule = nullptr;
        mparser_result* cResult = nullptr;
        const std::string cSource = "plot(1:3);";
        require(mparser_module_compile_utf8(cSource.data(), cSource.size(), nullptr, 0, &cModule)
            == MPARSER_API_STATUS_OK);
        require(mparser_module_execute(cModule, nullptr, &cResult) == MPARSER_API_STATUS_OK);
        mparser_module_release(cModule);
        require(mparser_result_succeeded(cResult));
        mparser_utf8_view identifiedView{};
        require(mparser_result_graphics_json_versioned(cResult, 3, &identifiedView) == MPARSER_API_STATUS_OK);
        require(nlohmann::json::parse(std::string(identifiedView.data, identifiedView.size)).at("version") == 3);
        require(mparser_result_graphics_json_versioned(cResult, 2, &identifiedView) == MPARSER_API_STATUS_INVALID_ARGUMENT);
        require(identifiedView.size == 0);
        require(mparser_result_graphics_json_versioned(nullptr, 3, &identifiedView) == MPARSER_API_STATUS_INVALID_ARGUMENT);
        require(identifiedView.size == 0);
        require(mparser_result_graphics_json_versioned(cResult, 3, nullptr) == MPARSER_API_STATUS_INVALID_ARGUMENT);
        const auto cSnapshot = mparser_result_graphics_json(cResult);
        require(nlohmann::json::parse(std::string(cSnapshot.data, cSnapshot.size))
            .at("objects").size() == 3);
        mparser_result_release(cResult);
        Value retained;
        Value retainedArray;
        {
            auto result = Module::compile(
                "h=plot([1,2]); h.DisplayName='response'; ax=gca(); g=legend(ax);"
                "g.Location='west'; legend hide; hidden=strcmp(g.Visible,'off');"
                "u=legend('show'); same=u==g; legend off; removed=~isvalid(g);"
                "kept=isvalid(h); replacement=legend();", "legend.m").execute();
            require(result.succeeded());
            size_t checks = 0;
            for (const auto& variable : result.variables()) {
                if (variable.name == "hidden" || variable.name == "same" || variable.name == "removed" || variable.name == "kept") {
                    const auto bits = variable.value.numericData<uint8_t>();
                    require(bits.size() == 1 && bits.front() == 1);
                    ++checks;
                }
            }
            require(checks == 4);
            const auto graph = nlohmann::json::parse(result.graphicsJson());
            const auto& objects = graph.at("objects");
            require(graph.at("version") == 2 && objects.size() == 4);
            require(objects[3].at("entries")[0].at("line") == objects[2].at("id"));
            require(objects[3].at("entries")[0].at("label") == "response");
        }
        {
            auto result = Module::compile("plot([1,2]); t=title('Measured gain'); xlabel('Time');", "labels.m").execute();
            require(result.succeeded());
            const auto graph = nlohmann::json::parse(result.graphicsJson());
            require(graph.at("version") == 2);
            const auto& objects = graph.at("objects");
            require(objects.size() == 5);
            require(objects[1].at("labels").at("Title") == objects[3].at("id"));
            require(objects[1].at("labels").at("XLabel") == objects[4].at("id"));
            require(objects[3].at("type") == "Text");
            require(objects[3].at("properties").at("String") == "Measured gain");
            bool found = false;
            for (const auto& variable : result.variables()) {
                if (variable.name != "t") { continue; }
                const auto value = nlohmann::json::parse(variable.value.graphicsJson());
                require(value.at("valid") == true && value.at("reference") == objects[3].at("id"));
                found = true;
            }
            require(found);
        }
        {
            auto runtime = Runtime::create();
            auto result = runtime.execute(Module::compile(
                "lines=plot([1,10;2,20;3,30]); ax=gca(); ax.YLim=[0,5];"
                "ax.Clipping=false; grid(ax,'on'); first=lines(1); first.Clipping=0;", "matrix.m"));
            require(result.succeeded());
            bool checked = false;
            for (const auto& variable : result.variables()) {
                if (variable.name != "lines") { continue; }
                const auto snapshot = nlohmann::json::parse(variable.value.graphicsJson());
                require(snapshot.at("dimensions") == nlohmann::json::array({2, 1}));
                require(snapshot.at("graphs").size() == 1);
                require(snapshot.at("elements").size() == 2);
                require(snapshot.at("elements")[0] != snapshot.at("elements")[1]);
                require(snapshot.at("elements")[0].at("valid") == true);
                require(snapshot.at("elements")[1].at("valid") == true);
                checked = true;
            }
            require(checked);
            const auto objects = nlohmann::json::parse(result.graphicsJson()).at("objects");
            require(objects.size() == 4);
            require(objects[1].at("properties").at("Clipping") == "off");
            require(objects[1].at("properties").at("XGrid") == "on");
            require(objects[1].at("properties").at("YGrid") == "on");
            require(objects[2].at("properties").at("Clipping") == "off");
            require(objects[3].at("properties").at("Clipping") == "on");
            require(objects[2].at("properties").at("YData") == nlohmann::json::array({1, 2, 3}));
            require(objects[3].at("properties").at("YData") == nlohmann::json::array({10, 20, 30}));
        }
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
        const auto identified = nlohmann::json::parse(retained.graphicsJson(3));
        require(identified.at("graph").at("version") == 3);
        require(identified.at("graph").at("graphIdentity").is_string());
        require(identified.at("graph").at("objects")[2].at("objectIdentity").is_string());
        const auto identifiedArray = nlohmann::json::parse(retainedArray.graphicsJson(3));
        require(identifiedArray.at("graphs")[0] == identified.at("graph"));
        require(identifiedArray.at("elements") == arraySnapshot.at("elements"));
        require(retained.graphicsJson() == retained.graphicsJson(0));
        require(initial.at("valid") == true && initial.at("reference") == 3);
        require(initial.at("graph").at("objects").size() == 3);
        require(initial.at("graph").at("objects")[2]["properties"]["YData"] == nlohmann::json::array({2,4,6}));
        mparser_value* rawSnapshot = nullptr;
        require(mparser_value_graphics_json(nullptr, &rawSnapshot) == MPARSER_API_STATUS_INVALID_ARGUMENT);
        require(rawSnapshot == nullptr);
        require(mparser_value_graphics_json(nullptr, nullptr) == MPARSER_API_STATUS_INVALID_ARGUMENT);
        mparser_value* missing = nullptr;
        require(mparser_value_create_missing(&missing) == MPARSER_API_STATUS_OK);
        require(mparser_value_graphics_json_versioned(missing, 2, &rawSnapshot) == MPARSER_API_STATUS_INVALID_ARGUMENT);
        require(rawSnapshot == nullptr);
        require(mparser_value_graphics_json_versioned(missing, 3, &rawSnapshot) == MPARSER_API_STATUS_TYPE_MISMATCH);
        require(rawSnapshot == nullptr);
        require(mparser_value_graphics_json_versioned(nullptr, 3, nullptr) == MPARSER_API_STATUS_INVALID_ARGUMENT);
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
