#include "legend_creation_checks.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/runtime/core/object_model/runtime_graphics.h"
#include <nlohmann/json-schema.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>

using namespace mparser;
int main(int argc,char** argv) {
    try {
        if (argc!=4) { throw std::runtime_error("expected Legend creation reference and v3/v4 schemas"); }
        std::ifstream input(argv[1]);const auto reference=nlohmann::json::parse(input);
        nlohmann::json_schema::json_validator v3,v4;
        std::ifstream schema3(argv[2]),schema4(argv[3]);
        v3.set_root_schema(nlohmann::json::parse(schema3));v4.set_root_schema(nlohmann::json::parse(schema4));
        const auto seed="f=figure; f.Visible='off'; ax=gca; a=plot(1:2); hold on; b=plot(2:3); c=plot(3:4);";
        size_t checked=0;
        for (const bool hir:{true,false}) {
            for (const auto& entry:reference.at("cases")) {
                const auto name=entry.at("name").get<std::string>();
                const auto state=std::make_shared<RuntimeSessionState>();
                const auto module=CompiledModule::compile(seed+entry.at("source").get<std::string>());
                if (!module.valid()) { throw std::runtime_error(name+": compilation"); }
                bool succeeded=false;
                bool warned=false;
                std::string messages;
                const auto record=[&](const auto& diagnostics) {
                    for (const auto& item:diagnostics) {
                        messages+=item.message+'\n';
                        warned|=item.identifier=="MATLAB:legend:IgnoringExtraEntries";
                    }
                };
                if (hir) {
                    InterpreterOptions options;options.sessionState=state;
                    auto result=Interpreter{}.run(module.semantic(),options);
                    record(result.diagnostics);
                    succeeded=std::none_of(result.diagnostics.begin(),result.diagnostics.end(),[](const auto& item) {return item.severity==DiagnosticSeverity::Error;});
                } else {
                    auto result=module.createSession(state).execute();
                    succeeded=result.succeeded();record(result.diagnostics);
                }
                if (succeeded!=entry.at("succeeded").get<bool>()) {
                    std::cerr<<messages;
                    throw std::runtime_error(name+": execution status");
                }
                const auto graph=state->graphicsGraph();
                auto json=nlohmann::json::parse(graph->serialize(true));checkLegendCreation(json,entry);v3.validate(json);
                v4.validate(nlohmann::json::parse(graph->serialize(true,false,true)));
                const auto original=std::find_if(graph->nodes().begin(),graph->nodes().end(),[](const auto& item) {return item.second.kind==RuntimeGraphicsKind::Axes;});
                if ((graph->currentAxes()==original->first)!=entry.at("global_current_is_original").get<bool>()) {
                    throw std::runtime_error(name+": current Axes changed");
                }
                if (succeeded && entry.at("warning")=="MATLAB:legend:IgnoringExtraEntries" &&
                    !warned) {
                    throw std::runtime_error(name+": extra-label warning missing");
                }
                ++checked;
            }
        }
        std::cout<<"PASS Legend creation: "<<checked<<" actual R2024b HIR/VM outcomes, membership/names/current Axes, v3/v4 schema and warning checks\n";
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
