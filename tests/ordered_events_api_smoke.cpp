#include "mparser/cpp_api.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace mparser::sdk;
using Json = nlohmann::json;
void require(bool condition,const char* message) { if (!condition) { throw std::runtime_error(message); } }
Value variable(const Result& result,const std::string& name) {
    for (const auto& value : result.variables()) { if (value.name == name) { return value.value; } }
    throw std::runtime_error("missing variable "+name);
}
double scalar(const Value& value) { return value.numericData().front(); }
const ExecutionEvent& labelled(const std::vector<ExecutionEvent>& events,std::string_view label) {
    const auto found = std::find_if(events.begin(),events.end(),[&](const auto& event){return event.label == label;});
    require(found != events.end(),"missing labelled event"); return *found;
}
void check(Backend backend) {
    const auto module = Module::compile("a=1\na=2;\ndisp('middle');\nwarning('Events:Notice','notice');\na+10\n"
        "error('Events:Stop','stop');\na=99", "ordered_events.m");
    Invocation request; request.backend = backend;
    require(module.execute(request).executionEvents().empty(),"default result remains unchanged");
    request.eventCapture = EventCaptureOptions{};
    auto result = module.execute(request);
    const auto events = result.executionEvents();
    const auto summary = result.eventCaptureSummary();
    require(!result.succeeded() && events.size() == 6 && summary.enabled && summary.started && !summary.truncated &&
        summary.generatedCount == 6,"ordered pre-failure events delivered through SDK");
    for (size_t index = 0; index < events.size(); ++index) {
        require(events[index].sequence == index && events[index].sourceContext != 0 &&
            events[index].source.sourceName == "ordered_events.m","events retain actual order and source");
    }
    require(events[0].kind == ExecutionEventKind::Assignment && events[0].label == "a" && events[0].text == "1" &&
        events[1].outputSuppressed && events[1].text == "2" && events[2].kind == ExecutionEventKind::Text &&
        events[3].kind == ExecutionEventKind::Warning && events[3].diagnostic->identifier == "Events:Notice" &&
        events[4].kind == ExecutionEventKind::Expression && events[4].label == "ans" &&
        events[5].diagnostic->identifier == "Events:Stop" && events[5].disposition == ExecutionEventDisposition::Unhandled,
        "event families, real labels, suppression and diagnostic disposition");
    result = {};
    require(Json::parse(events[0].valueSnapshotJson).at("nodes").size() == 1,"C++ event copies own their immutable data");
    request.eventCapture->maximumEvents = 2;
    result = module.execute(request);
    require(result.executionEvents().size() == 2 && result.eventCaptureSummary().truncated &&
        result.eventCaptureSummary().truncationReason == EventTruncationReason::EventCount &&
        scalar(variable(result,"a")) == 2,"count truncation is explicit and does not change computation");
    request.eventCapture = EventCaptureOptions{};
    request.eventCapture->maximumBytes = 0;
    result = module.execute(request);
    require(result.executionEvents().empty() && result.eventCaptureSummary().truncated &&
        result.eventCaptureSummary().truncationReason == EventTruncationReason::ByteLimit,"zero return budget is not a fake complete stream");
    const auto capture = Module::compile("text=evalc('x=1; x=2; x+3'); tail=9;","capture.m");
    SystemContextOptions contextOptions;
    contextOptions.rootDirectory = std::filesystem::current_path().generic_string();
    contextOptions.capabilities = SystemCapability::DynamicEvaluation;
    const auto context = SystemContext::rootedNative(contextOptions);
    request.eventCapture = EventCaptureOptions{};
    const auto full = capture.execute(request,context);
    request.eventCapture->maximumEvents = 1;
    const auto bounded = capture.execute(request,context);
    require(full.succeeded() && bounded.succeeded(),"evalc fixture executes with its explicit capability");
    const auto fullText = variable(full,"text"), boundedText = variable(bounded,"text");
    require(std::equal(fullText.characterData().begin(),fullText.characterData().end(),
        boundedText.characterData().begin(),boundedText.characterData().end()) &&
        scalar(variable(bounded,"tail")) == 9,"optional result truncation never changes evalc semantic output");
    request.eventCapture = EventCaptureOptions{};
    request.eventCapture->maximumValueBytes = 1;
    result = Module::compile("x=4","small.m").execute(request);
    require(result.succeeded() && result.executionEvents().size() == 1 &&
        !result.executionEvents()[0].valueUnavailable.empty() && result.executionEvents()[0].valueSnapshotJson.empty(),
        "value limit is explicit unavailability, not an empty or final-workspace replacement");
}
void privacy() {
    const auto module = Module::compile(R"(h=ProtectedSnapshot();
p=addprop(h,'DynamicVisible');
h.DynamicVisible=33;
p.GetAccess='private';
h
secret=71234567; f=@(x)x+secret;
classdef ProtectedSnapshot < SnapshotBase
properties
Visible=7
Self=[]
Wrapped=5
end
properties (GetAccess=private)
Secret=91234567
end
properties (Hidden)
HiddenValue=81234567
end
properties (Dependent)
Derived
end
methods
function value=get.Derived(obj)
error('Snapshot:Getter','snapshot must not evaluate getters');
value=obj.Secret;
end
function value=get.Wrapped(obj)
error('Snapshot:Getter','snapshot must not evaluate getters');
value=obj.Secret;
end
end
end
classdef SnapshotBase < dynamicprops
properties
Inherited=11
end
properties (GetAccess=protected)
ProtectedValue=61234567
end
end
)","privacy.m");
    require(module.isValid(),"privacy fixture compiles");
    for (const auto backend : {Backend::Bytecode,Backend::Portable,Backend::Native,Backend::Automatic}) {
        Invocation request; request.backend = backend; request.eventCapture = EventCaptureOptions{};
        const auto result = module.execute(request);
        require(result.succeeded(),"snapshot executes no dependent getter");
        const auto events = result.executionEvents();
        const auto& object = labelled(events,"h").valueSnapshotJson;
        require(object.find("Secret") == std::string::npos && object.find("HiddenValue") == std::string::npos &&
            object.find("ProtectedValue") == std::string::npos && object.find("Inherited") != std::string::npos &&
            object.find("Visible") != std::string::npos && object.find("dependent-not-evaluated") != std::string::npos &&
            object.find("getter-not-evaluated") != std::string::npos,
            "SDK snapshot follows external visibility instead of dumping internal storage");
        std::vector<ExecutionEvent> objects;
        for (const auto& event : events) { if (event.label == "h") { objects.push_back(event); } }
        require(objects.size() == 3,"object constructor, dynamic write and later reference are recorded");
        const auto dynamic = Json::parse(objects[1].valueSnapshotJson);
        const auto& properties = dynamic.at("nodes").at(0).at("properties");
        const auto item = std::find_if(properties.begin(),properties.end(),[](const auto& property){return property.at("name") == "DynamicVisible";});
        require(item != properties.end() && item->contains("value"),"public dynamic properties use actual kernel storage");
        const auto bits = dynamic.at("nodes").at(item->at("value").get<size_t>()).at("numeric").at("real_bits").at(0).get<std::string>();
        require(std::bit_cast<double>(std::stoull(bits,nullptr,16)) == 33,"dynamic value is frozen before access changes");
        require(objects[2].valueSnapshotJson.find("DynamicVisible") == std::string::npos,
            "later private access does not retroactively change old public history");
        const auto& closure = labelled(events,"f").valueSnapshotJson;
        require(closure.find("secret") == std::string::npos && closure.find("71234567") == std::string::npos,
            "SDK closure descriptor exposes no captured variables");
        require(labelled(events,"f").text.find("secret") == std::string::npos,
            "automatic display text applies the same closure privacy policy as JSON");
        const auto& descriptor = labelled(events,"p");
        require(descriptor.text.find("::") == std::string::npos &&
            descriptor.valueSnapshotJson.find("public-properties") != std::string::npos &&
            descriptor.valueSnapshotJson.find("Name") != std::string::npos,
            "native property descriptors expose public fields without internal display storage keys");
    }
}
void semanticCapture() {
    SystemContextOptions options;
    options.rootDirectory = std::filesystem::current_path().generic_string();
    options.capabilities = SystemCapability::DynamicEvaluation;
    const auto context = SystemContext::rootedNative(options);
    for (const auto& source : {
        "text=evalc('local=9'); tail=9;",
        "text=evalc('v=0; for k=1:130, v={v}; end; v'); tail=9;",
        "text=evalc('callback()'); tail=9;\nfunction callback()\nlocal=9\nend",
    }) {
        const auto module = Module::compile(source,"semantic_capture.m");
        Invocation request;
        const auto plain = module.execute(request,context);
        request.eventCapture = EventCaptureOptions{}; request.eventCapture->maximumEvents = 0;
        const auto recorded = module.execute(request,context);
        require(plain.succeeded() && recorded.succeeded(),"optional snapshot limits must not fail otherwise valid evalc");
        const auto before = variable(plain,"text"), after = variable(recorded,"text");
        require(!before.characterData().empty() && std::equal(before.characterData().begin(),before.characterData().end(),
            after.characterData().begin(),after.characterData().end()),"evalc semantic output is independent of opt-in recording");
        require(scalar(variable(recorded,"tail")) == 9,"evalc continues past optional snapshot failures");
    }
    Invocation request; request.eventCapture = EventCaptureOptions{};
    const auto result = Module::compile("e=MException('Events:Native','message'); wrapped={e};","native.m").execute(request);
    const auto events = result.executionEvents();
    require(labelled(events,"e").valueSnapshotJson.find("identifier") != std::string::npos,
        "native exception stored public properties are inspectable");
    const auto nested = Module::compile("secret=7; f=@(x)x+secret; wrapped={f,struct('callback',f)};","nested_privacy.m").execute(request);
    require(labelled(nested.executionEvents(),"wrapped").text.find("secret") == std::string::npos,
        "nested container display cannot bypass capture redaction");
}
void ownership() {
    auto runtime = Runtime::create();
    auto producer = Module::compile("function object=make_foreign()\nobject=SameName();\nend\n"
        "classdef SameName < handle\nproperties (GetAccess=private)\nSecret=91234567\nend\nend","producer.m");
    Invocation request; request.entryFunction = "make_foreign"; request.requestedOutputCount = 1;
    auto exported = runtime.execute(producer,request);
    require(exported.succeeded(),"foreign class fixture executes");
    auto consumer = Module::compile("forwarded=foreign;\nclassdef SameName < handle\nproperties\nSecret=1\nend\nend","consumer.m");
    request = {}; request.eventCapture = EventCaptureOptions{}; request.initialWorkspace = {{"foreign",exported.output(0)}};
    auto result = runtime.execute(consumer,request);
    auto events = result.executionEvents();
    require(result.succeeded() && events.size() == 1 && events[0].valueSnapshotJson.find("Secret") == std::string::npos &&
        events[0].valueSnapshotJson.find("opaque") != std::string::npos,
        "same-name consumer class cannot authorize reading another module's private storage");
    result = {}; exported = {}; producer = {}; consumer = {}; runtime = {};
    require(!events[0].valueSnapshotJson.empty(),"owned event data survives all modules/runtime handles");
    auto module = Module::compile("function result=next()\npersistent counter;\nif isempty(counter), counter=0; end\n"
        "counter=counter+1;\nresult=counter\nend","session.m");
    auto session = module.createSession();
    request = {}; request.entryFunction = "next"; request.requestedOutputCount = 1; request.eventCapture = EventCaptureOptions{};
    const auto first = session.execute(request), second = session.execute(request);
    require(scalar(first.output(0)) == 1 && scalar(second.output(0)) == 2 && !first.executionEvents().empty() &&
        first.executionEvents()[0].sequence == 0 && second.executionEvents()[0].sequence == 0,
        "session entry path returns independent ordered collections on repeated calls");
}
void cApi() {
    mparser_module* module = nullptr;
    const std::string source = "a=1";
    require(mparser_module_compile_utf8(source.data(),source.size(),"c.m",3,&module) == MPARSER_API_STATUS_OK,"C fixture compiles");
    mparser_invocation_options options{};
    require(MPARSER_INVOCATION_OPTIONS_INIT(&options) == MPARSER_API_STATUS_OK,"C options initialize");
    options.struct_size = static_cast<uint32_t>(offsetof(mparser_invocation_options,event_capture));
    options.event_capture = reinterpret_cast<const mparser_event_capture_options*>(static_cast<uintptr_t>(1));
    mparser_result* result = nullptr;
    require(mparser_module_execute(module,&options,&result) == MPARSER_API_STATUS_OK &&
        mparser_result_execution_event_count(result) == 0,"old option prefix never reads the new tail");
    mparser_result_release(result);
    require(MPARSER_INVOCATION_OPTIONS_INIT(&options) == MPARSER_API_STATUS_OK,"full C options initialize");
    mparser_event_capture_options capture{};
    require(MPARSER_EVENT_CAPTURE_OPTIONS_INIT(&capture) == MPARSER_API_STATUS_OK,"event options initialize");
    options.event_capture = &capture;
    require(mparser_module_execute(module,&options,&result) == MPARSER_API_STATUS_OK &&
        mparser_result_execution_event_count(result) == 1,"C capture is explicit and additive");
    mparser_execution_event_info info{}; info.sequence = 999;
    require(mparser_result_execution_event_info(result,0,&info,0) == MPARSER_API_STATUS_INVALID_ARGUMENT && info.sequence == 999,
        "failed sized event query preserves caller output");
    require(mparser_result_execution_event_info(result,9,&info,sizeof(info)) == MPARSER_API_STATUS_OUT_OF_RANGE && info.sequence == 999,
        "bad event index preserves caller output");
    require(mparser_result_execution_event_info(result,0,&info,sizeof(info)) == MPARSER_API_STATUS_OK && info.sequence == 0 &&
        info.value_snapshot_json.size != 0,"C event exposes borrowed read-only views");
    mparser_result_release(result); result = nullptr;
    --capture.struct_size;
    require(mparser_module_execute(module,&options,&result) == MPARSER_API_STATUS_ABI_MISMATCH && result == nullptr,
        "undersized capture configuration is rejected before execution");
    mparser_module_release(module);
}
}
int main() {
    try {
        for (const auto backend : {Backend::Bytecode,Backend::Portable,Backend::Native,Backend::Automatic}) { check(backend); }
        semanticCapture(); privacy(); ownership(); cApi(); std::cout << "ordered read-only C/C++ event collection passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
