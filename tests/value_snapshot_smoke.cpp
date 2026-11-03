#include "mparser/runtime/core/value/runtime_value_snapshot.h"
#include "mparser/runtime/core/session/runtime_output.h"
#include "mparser/runtime/core/object_model/runtime_graphics.h"
#include "mparser/runtime/core/object_model/runtime_metadata.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_table.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_timetable.h"

#include <algorithm>
#include <bit>
#include <iostream>
#include <limits>
#include <type_traits>

namespace {
using namespace mparser;
static_assert(std::is_same_v<decltype(RuntimeValueSnapshot{}.node()), const RuntimeSnapshotNode&>);

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
RuntimeSnapshotId field(const std::vector<RuntimeSnapshotField>& fields, const std::string& name) {
    const auto found = std::find_if(fields.begin(),fields.end(),
        [&](const auto& item) { return item.first == name; });
    require(found != fields.end(), "snapshot field missing");
    return found->second;
}
void fails(const RuntimeValue& value, RuntimeSnapshotLimits limits, RuntimeSnapshotFailure reason) {
    bool rejected = false;
    try { (void)RuntimeValueSnapshot::capture(value,limits); }
    catch (const RuntimeSnapshotError& error) { rejected = error.reason() == reason; }
    require(rejected,"snapshot must report its explicit failure reason");
}

void numericAndText() {
    auto matrix = makeRuntimeMatrixValue(2,2,{1,2,3,4});
    auto snapshot = RuntimeValueSnapshot::capture(matrix);
    require(snapshot.node().dimensions == std::vector<size_t>({2,2}), "numeric shape retained");
    for (size_t index=0; index<4; ++index) {
        require(snapshot.node().numeric[index].real == runtimeNumericElementValue(matrix,index)->real,
            "numeric snapshot uses logical column-major order");
    }
    matrix.elements.assign(4,999);
    require(snapshot.node().numeric[0].real == 1,"dense snapshot detached from live storage");
    RuntimeNumericElementValue integer;
    integer.numericClass = RuntimeNumericClass::UInt64;
    integer.integerRealBits = std::numeric_limits<std::uint64_t>::max();
    integer.integerImaginaryBits = 9007199254740993ULL;
    integer.complex = true;
    auto exact = runtimeNumericValueFromElements({1,1},{integer},RuntimeNumericClass::UInt64);
    require(exact.has_value(),"exact integer fixture");
    auto exactSnapshot = RuntimeValueSnapshot::capture(*exact);
    const auto& captured = exactSnapshot.node().numeric.front();
    require(captured.integerRealBits == integer.integerRealBits &&
        captured.integerImaginaryBits == integer.integerImaginaryBits && captured.complex,
        "uint64 complex values retain exact real and imaginary bits");
    for (const auto bits : {0x8000000000000000ULL,0x7ff8000000001234ULL,0x7ff0000000000000ULL}) {
        const auto value = RuntimeValueSnapshot::capture(makeRuntimeNumberValue(std::bit_cast<double>(bits)));
        require(std::bit_cast<std::uint64_t>(value.node().numeric[0].real) == bits,
            "binary64 signed zero NaN payload and infinity bits retained");
    }
    auto character = makeRuntimeCharacterVector(u"A\u8def\U0001f600");
    auto chars = RuntimeValueSnapshot::capture(character);
    require(chars.node().characters == u"A\u8def\U0001f600", "UTF-16 including surrogate pair retained");
    auto strings = makeRuntimeStringArray({1,2},{{u"hello",false},{u"",true}});
    auto texts = RuntimeValueSnapshot::capture(strings);
    strings.stringElements[0].value = u"changed";
    require(texts.node().strings[0].value == u"hello" && texts.node().strings[1].missing,
        "string missingness and characters detached");
    require(RuntimeValueSnapshot::capture(makeRuntimeMissingValue()).node().kind == RuntimeValueKind::Missing,
        "missing value retains kind");
    require(RuntimeValueSnapshot::capture(makeRuntimeVectorValue({})).node().numeric.empty(),
        "empty is a valid snapshot, not unavailable");
}

void containersAndCycles() {
    auto handle = makeRuntimeObjectScalar("HistoryHandle",{{"value",makeRuntimeNumberValue(4)}},true);
    (*handle.sharedFields)["self"] = handle;
    auto root = makeRuntimeCellValue({handle,handle,makeRuntimeStructValue({{"entry",handle}})});
    auto snapshot = RuntimeValueSnapshot::capture(root);
    const auto first = snapshot.node().elements[0];
    require(first == snapshot.node().elements[1],"handle aliases retain one snapshot-local identity");
    require(field(snapshot.node(first).storedFields,"self") == first,"self cycle represented by an ID edge");
    const auto structure = snapshot.node().elements[2];
    require(field(snapshot.node(structure).structElements[0],"entry") == first,
        "aliases preserved through nested struct/cell containers");
    (*handle.sharedFields)["value"] = makeRuntimeNumberValue(9);
    require(snapshot.node(field(snapshot.node(first).storedFields,"value")).numeric[0].real == 4,
        "shared object mutation cannot rewrite history");
    auto copied = snapshot;
    std::weak_ptr<RuntimeWorkspace> lifetime = handle.sharedFields;
    handle.sharedFields->erase("self");
    root = {};
    handle = {};
    require(lifetime.expired(),"snapshot retains no live shared object fields");
    snapshot = {};
    require(copied.node(first).className == "HistoryHandle", "snapshot copies own independent shared immutable data");
    auto argument = makeRuntimeNameValueArgument("option",makeRuntimeNumberValue(3));
    auto named = RuntimeValueSnapshot::capture(argument);
    require(named.node().text == "option" && named.node(named.node().elements[0]).numeric[0].real == 3,
        "named argument preserves label and value");
    auto weakOwner = std::make_shared<RuntimeWorkspace>();
    auto descriptor = makeRuntimeMetadataObject(RuntimeMetadataKind::DynamicProperty,"extra");
    descriptor.opaqueId=7;
    descriptor.sharedFields=std::make_shared<RuntimeWorkspace>();
    descriptor.dynamicPropertyOwner=std::make_shared<RuntimeDynamicPropertyOwner>();
    descriptor.dynamicPropertyOwner->className="Owner";
    descriptor.dynamicPropertyOwner->fields=weakOwner;
    auto live = RuntimeValueSnapshot::capture(descriptor);
    weakOwner.reset();
    auto expired = RuntimeValueSnapshot::capture(descriptor);
    require(live.node().weakOwner->alive && !expired.node().weakOwner->alive &&
        live.node().weakOwner->className == "Owner", "weak owners freeze liveness without retaining owners");
}

void richValues() {
    auto sparseStorage = std::make_shared<RuntimeSparseStorage>();
    sparseStorage->rows=100000000; sparseStorage->columns=1;
    sparseStorage->columnPointers={0,1}; sparseStorage->rowIndices={99999999}; sparseStorage->values={2};
    auto sparse = makeRuntimeSparseValue(sparseStorage);
    auto frozenSparse = RuntimeValueSnapshot::capture(sparse);
    sparseStorage->values[0] = 5;
    require(frozenSparse.node().numeric.empty() && frozenSparse.node().sparse->values[0] == 2 &&
        frozenSparse.node().sparse->rowIndices[0] == 99999999,
        "large sparse snapshot preserves CSC without dense expansion or shared mutable storage");
    auto categorical = runtimeMakeCategoricalValue({2,1},{"red","blue"},{1,0},true,true);
    require(categorical.succeeded,"categorical fixture");
    auto frozenCategory = RuntimeValueSnapshot::capture(categorical.value);
    categorical.value.categoricalStorage->categories[0]="changed";
    require(frozenCategory.node().categorical->categories[0] == "red" &&
        frozenCategory.node().categorical->codes == std::vector<std::uint32_t>({1,0}) &&
        frozenCategory.node().categorical->ordinal && frozenCategory.node().categorical->protectedCategories,
        "categorical dictionary codes missingness and flags retained");
    auto temporal = runtimeMakeTemporalValue(RuntimeTemporalKind::Duration,{2,1},{3,4});
    require(temporal.succeeded,"temporal fixture");
    auto duration = RuntimeValueSnapshot::capture(temporal.value);
    require(duration.node().className == "duration" && duration.node().temporal == std::vector<double>({3,4}),
        "temporal arrays retain their real payloads");
    auto metadata = makeRuntimeMetadataArray(RuntimeMetadataKind::Class,{
        makeRuntimeMetadataObject(RuntimeMetadataKind::Class,"First"),
        makeRuntimeMetadataObject(RuntimeMetadataKind::Class,"Second")});
    auto meta = RuntimeValueSnapshot::capture(metadata);
    require(meta.node(meta.node().elements[1]).text == "Second", "metadata descriptor arrays retained without lookup");
    auto table = runtimeMakeTable({makeRuntimeVectorValue({1,2})},{"v"});
    require(table.succeeded,"table fixture");
    table.value.tabularStorage->description="original";
    table.value.tabularStorage->userData = table.value;
    auto frozenTable = RuntimeValueSnapshot::capture(table.value);
    table.value.tabularStorage->description="changed";
    table.value.tabularStorage->variables[0].value = makeRuntimeVectorValue({9,9});
    require(frozenTable.node().tabular->description == "original" && frozenTable.node().tabular->userData == 0,
        "table properties and self-referential UserData frozen");
    require(frozenTable.node(frozenTable.node().tabular->variables[0].second).numeric[0].real == 1,
        "table variables detached");
    table.value.tabularStorage->userData = {};
    auto timetable = runtimeMakeTimetable(temporal.value,{makeRuntimeMatrixValue(2,1,{5,6})},{"x"});
    require(timetable.succeeded,"timetable fixture");
    auto times = RuntimeValueSnapshot::capture(timetable.value);
    require(times.node().tabular->kind == RuntimeTabularKind::Timetable &&
        times.node(times.node().tabular->rowTimes).temporal == std::vector<double>({3,4}),
        "timetable row axis and variables retained");
}

void callablesAndGraphics() {
    auto context = makeRuntimeCallableContext();
    std::weak_ptr<RuntimeCallableContext> contextLifetime = context;
    RuntimeFunctionHandle handle;
    handle.kind = RuntimeFunctionHandleKind::Anonymous;
    handle.backend = RuntimeFunctionHandleBackend::Bytecode;
    handle.context = context;
    handle.display="@(x)x+offset";
    handle.parameters={"x"};
    handle.capturedVariables={{"offset",makeRuntimeNumberValue(3)}};
    auto callable = makeRuntimeFunctionHandleValue(std::move(handle));
    callable.functionHandle->capturedVariables["self"] = callable;
    auto snapshot = RuntimeValueSnapshot::capture(callable);
    require(snapshot.node().callable->moduleBound && snapshot.node().callable->parameters[0] == "x" &&
        field(snapshot.node().callable->captures,"self") == 0,"callable description and cyclic captures frozen");
    callable.functionHandle->capturedVariables.clear(); callable={}; context.reset();
    require(contextLifetime.expired(),"snapshot cannot keep executable code/context alive");
    auto graph = std::make_shared<RuntimeGraphicsGraph>();
    auto lineId = graph->plot({1,2});
    auto value = makeRuntimeGraphicsValue(RuntimeGraphicsHandle(graph,lineId));
    auto graphics = RuntimeValueSnapshot::capture(value);
    const auto json = graphics.node().graphicsJson;
    require(json == serializeRuntimeGraphicsValue(value,true),"graphics v3 reference and graph frozen together");
    value.graphicsHandle->setProperty("Color",std::vector<double>{1,0,0});
    require(serializeRuntimeGraphicsValue(value,true) != json && graphics.node().graphicsJson == json,
        "later graphics property changes cannot rewrite history");
    value.graphicsHandle->erase();
    auto deleted = RuntimeValueSnapshot::capture(value);
    require(deleted.node().graphicsJson.find("\"valid\":false") != std::string::npos,
        "deleted handles have an explicit invalid snapshot");
    std::weak_ptr<RuntimeGraphicsGraph> graphLifetime=graph;
    value={}; graph.reset();
    require(graphLifetime.expired() && graphics.node().graphicsJson == json,
        "snapshot does not keep live graphics graph/handles alive");
}

void limitsAndFailures() {
    auto child = makeRuntimeCellValue({makeRuntimeNumberValue(1)});
    RuntimeSnapshotLimits limits;
    limits.maximumDepth=0; fails(child,limits,RuntimeSnapshotFailure::DepthLimit);
    limits={}; limits.maximumNodes=1; fails(child,limits,RuntimeSnapshotFailure::NodeLimit);
    limits={}; limits.maximumBytes=0; fails(child,limits,RuntimeSnapshotFailure::ByteLimit);
    auto invalid = makeRuntimeMatrixValue(2,2,{1,2,3,4}); invalid.elements.clear();
    fails(invalid,{},RuntimeSnapshotFailure::InvalidValue);
    limits={}; limits.maximumNodes=1;
    auto event = runtimeCaptureAssignmentDisplay("a",child,{},"limits.m",false,7,{},limits);
    require(!event.valueSnapshot.available() && event.snapshotFailure == RuntimeSnapshotFailure::NodeLimit &&
        !event.snapshotError.empty() && event.sequence == 7 && event.text.empty(),
        "event reports missing structured snapshot without fake or unbounded fallback formatting");
    auto loop = makeRuntimeObjectScalar("Cycle",{},true);
    (*loop.sharedFields)["self"] = loop;
    limits={}; limits.maximumDepth=0; limits.maximumNodes=1;
    require(RuntimeValueSnapshot::capture(loop,limits).size() == 1,
        "self reference consumes no duplicate nodes or recursive depth");
    loop.sharedFields->clear();
    bool unavailable=false;
    try { (void)RuntimeValueSnapshot{}.node(); } catch(const std::out_of_range&) { unavailable=true; }
    require(unavailable,"unavailable is not confused with an empty value");
}
} // namespace

int main() {
    try {
        numericAndText(); containersAndCycles(); richValues(); callablesAndGraphics(); limitsAndFailures();
        std::cout << "immutable value snapshots: precise payloads, alias/cycle graphs, rich values, detached lifetimes and limits passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
