#include "legend_creation_checks.h"
#include "mparser/cpp_api.hpp"
#include <fstream>
#include <iostream>

using namespace mparser::sdk;
int main(int argc,char** argv) {
    try {
        if (argc!=2) { throw std::runtime_error("expected Legend creation reference"); }
        std::ifstream input(argv[1]);const auto reference=nlohmann::json::parse(input);
        const auto require=[](bool condition,const std::string& message) {if (!condition) {throw std::runtime_error(message);}};
        const auto seed="f=figure; f.Visible='off'; ax=gca; a=plot(1:2); hold on; b=plot(2:3); c=plot(3:4);";
        size_t checked=0;
        for (const auto backend:{Backend::Automatic,Backend::Bytecode,Backend::Portable,Backend::Native}) {
            for (const auto& entry:reference.at("cases")) {
                auto runtime=Runtime::create();Invocation request;request.backend=backend;
                const auto name=entry.at("name").get<std::string>();
                const auto source=seed+entry.at("source").get<std::string>();
                const auto result=runtime.execute(Module::compile(source,"legend_creation_api.m"),request);
                require(result.succeeded()==entry.at("succeeded").get<bool>(),name+": SDK execution");
                checkLegendCreation(nlohmann::json::parse(result.graphicsJson(3)),entry);
                checkLegendCreation(nlohmann::json::parse(result.graphicsJson(4)),entry);
                const auto frozen=result.graphicsJson(4);
                const auto next=runtime.execute(Module::compile("close all; plot(9:10);","legend_replace.m"),request);
                require(next.succeeded() && result.graphicsJson(4)==frozen,name+": historical Result changed");
                ++checked;
            }
            auto runtime=Runtime::create();Invocation request;request.backend=backend;
            const auto empty=runtime.graphicsJson(3);
            for (const auto* source:{"legend({'A'},'Location','bad');","legend({'A',42});","legend('AutoUpdate',2);"}) {
                const auto denied=runtime.execute(Module::compile(source,"legend_atomic.m"),request);
                require(!denied.succeeded() && runtime.graphicsJson(3)==empty,"invalid creation manufactured a Figure or Axes");
            }
            auto foreign=Runtime::create();const auto other=foreign.execute(Module::compile("h=plot(1:2);","legend_foreign.m"));
            request.initialWorkspace=other.variables();
            const auto denied=runtime.execute(Module::compile("legend(h);","legend_foreign_target.m"),request);
            require(!denied.succeeded() && runtime.graphicsJson(3)==empty,"foreign handle changed graph");
        }
        std::cout<<"PASS Legend creation SDK: "<<checked<<" R2024b outcomes in four requested backends, frozen v3/v4 history and atomic invalid/foreign rejection\n";
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
