#include "mparser/cpp_api.hpp"
#include <nlohmann/json.hpp>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace mparser::sdk;
using Json = nlohmann::json;
void require(bool value, const std::string& message) { if (!value) { throw std::runtime_error(message); } }
template<class Action> void rejects(Action action, ApiStatus status) {
    try { action(); } catch (const ApiError& error) { require(error.status()==status,"unexpected SDK rejection"); return; }
    throw std::runtime_error("expected SDK rejection");
}
int main() {
    try {
        for (const auto backend : {Backend::Automatic,Backend::Bytecode,Backend::Portable,Backend::Native}) {
            auto runtime=Runtime::create(); Invocation request; request.backend=backend;
            const auto seed=runtime.execute(Module::compile("seed=42; lines=plot([1,10;2,20]); first=lines(1); second=lines(2);", "identity_seed.m"),request);
            require(seed.succeeded(),"seed");
            const auto frozen=seed.graphicsJson(3);const auto graph=Json::parse(frozen);
            const auto graphId=graph.at("graphIdentity").get<std::string>();
            const auto firstId=graph.at("objects")[2].at("objectIdentity").get<std::string>();
            const auto secondId=graph.at("objects")[3].at("objectIdentity").get<std::string>();
            auto live=runtime.graphicsObject(graphId,secondId);
            require(Json::parse(live.graphicsJson(3)).at("graph")==graph,"resolved graph");
            const std::array<size_t,2> rgbShape{1,3};
            const std::array<double,3> rgb{1,0,0};
            const auto red=Value::numericArray<double>(NumericClass::Double,rgbShape,rgb);
            runtime.setGraphicsProperty(graphId,secondId,"Color",red);
            const std::array<size_t,2> labelShape{1,2};
            const std::array<std::uint16_t,2> label{0x6d4b,0x91cf};
            runtime.setGraphicsProperty(graphId,secondId,"DisplayName",Value::characterArray(labelShape,label));
            const auto changed=Json::parse(runtime.graphicsJson(3));
            require(changed.at("objects")[3].at("properties").at("Color")==Json::array({1,0,0}),"color write");
            require(changed.at("objects")[3].at("properties").at("DisplayName")=="\xe6\xb5\x8b\xe9\x87\x8f","Unicode JSON");
            require(seed.graphicsJson(3)==frozen,"frozen result");
            const auto before=runtime.graphicsJson(3);
            rejects([&]{runtime.setGraphicsProperty(graphId,secondId,"Color",Value::scalar(2));},MPARSER_API_STATUS_TYPE_MISMATCH);
            rejects([&]{runtime.setGraphicsProperty(graphId,secondId,"unknown",red);},MPARSER_API_STATUS_TYPE_MISMATCH);
            for(const auto id:{"","0","01","+1","-1","1 ","18446744073709551616"}) {
                rejects([&]{(void)runtime.graphicsObject(graphId,id);},MPARSER_API_STATUS_INVALID_ARGUMENT);
                rejects([&]{runtime.setGraphicsProperty(graphId,id,"Color",red);},MPARSER_API_STATUS_INVALID_ARGUMENT);
            }
            require(runtime.graphicsJson(3)==before,"invalid mutation changed graph");
            auto foreign=Runtime::create();
            rejects([&]{foreign.setGraphicsProperty(graphId,secondId,"Color",red);},MPARSER_API_STATUS_OWNER_MISMATCH);
            request.initialWorkspace=seed.variables();
            const auto removed=runtime.execute(Module::compile("delete(first); observed=second.Color;", "identity_delete.m"),request);
            require(removed.succeeded(),"delete first");
            const auto renumbered=Json::parse(runtime.graphicsJson(3));
            require(renumbered.at("objects").back().at("id")==3,"local id renumbering control");
            require(renumbered.at("objects").back().at("objectIdentity")==secondId,"stable identity");
            rejects([&]{runtime.setGraphicsProperty(graphId,firstId,"Color",red);},MPARSER_API_STATUS_OUT_OF_RANGE);
            runtime.setGraphicsProperty(graphId,secondId,"Clipping",Value::numericScalar<std::uint8_t>(NumericClass::Logical,0));
            auto reader=std::async(std::launch::async,[runtime,graphId]() {
                for(int index=0;index<100;++index) {
                    const auto snapshot=Json::parse(runtime.graphicsJson(3));
                    require(snapshot.at("graphIdentity")==graphId,"concurrent read identity");
                }
            });
            for(int index=0;index<100;++index) { runtime.setGraphicsProperty(graphId,secondId,"Color",red); }
            reader.get();
            auto resetWriter=std::async(std::launch::async,[runtime,graphId,secondId,red]() mutable {
                for(int index=0;index<100;++index) {
                    try { runtime.setGraphicsProperty(graphId,secondId,"Color",red); }
                    catch(const ApiError& error) { require(error.status()==MPARSER_API_STATUS_OWNER_MISMATCH,"reset-race rejection"); }
                }
            });
            runtime.reset();resetWriter.get();
            const auto reset=runtime.graphicsJson(3);
            require(Json::parse(reset).at("graphIdentity")!=graphId,"reset namespace");
            rejects([&]{runtime.setGraphicsProperty(graphId,secondId,"Color",red);},MPARSER_API_STATUS_OWNER_MISMATCH);
            rejects([&]{(void)runtime.graphicsObject(graphId,secondId);},MPARSER_API_STATUS_OWNER_MISMATCH);
            require(runtime.graphicsJson(3)==reset,"stale write changed new graph");
            require(Json::parse(live.graphicsJson(3)).at("valid")==true,"old retained graph control");
        }
        mparser_value* output=reinterpret_cast<mparser_value*>(1);
        require(mparser_runtime_graphics_json_versioned(nullptr,3,&output)==MPARSER_API_STATUS_INVALID_ARGUMENT && !output,"null runtime output");
        mparser_runtime* runtime=nullptr;
        require(mparser_runtime_create(nullptr,&runtime)==MPARSER_API_STATUS_OK,"C runtime");
        output=reinterpret_cast<mparser_value*>(1);
        require(mparser_runtime_graphics_json_versioned(runtime,2,&output)==MPARSER_API_STATUS_INVALID_ARGUMENT && !output,"unsupported version");
        output=reinterpret_cast<mparser_value*>(1);
        require(mparser_runtime_graphics_value(runtime,{nullptr,1},{"1",1},&output)==MPARSER_API_STATUS_INVALID_ARGUMENT && !output,"malformed view");
        require(mparser_runtime_graphics_value(runtime,{"1",1},{"1",1},nullptr)==MPARSER_API_STATUS_INVALID_ARGUMENT,"null output");
        mparser_runtime_release(runtime);
        std::cout<<"PASS identity-targeted graphics SDK: current graph, writes, Unicode, stale/foreign/deleted rejection and concurrent reads\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
