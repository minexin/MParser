#include "mparser/runtime/core/value/runtime_value_snapshot.h"
#include "mparser/runtime/core/value/runtime_snapshot_json.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include <nlohmann/json.hpp>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace mparser;
using Json = nlohmann::json;
void require(bool condition,const char* message) { if (!condition) { throw std::runtime_error(message); } }
Json project(const RuntimeValue& value,RuntimeSnapshotPropertyResolver resolver = {}) {
    return Json::parse(runtimePublicSnapshotJson(RuntimeValueSnapshot::capture(value,{},std::move(resolver))));
}
void privacy() {
    auto object = makeRuntimeObjectScalar("Visible",{{"public",makeRuntimeNumberValue(7)},
        {"secret",makeRuntimeNumberValue(91234567)},{"hidden",makeRuntimeNumberValue(81234567)}},true);
    (*object.sharedFields)["self"] = object;
    RuntimeSnapshotPropertyResolver properties = [](const RuntimeValue& value)
        ->std::optional<std::vector<RuntimeSnapshotPropertySpec>> {
        if (value.className != "Visible") { return std::nullopt; }
        return std::vector<RuntimeSnapshotPropertySpec>{{"PublicValue","public",{}},
            {"Self","self",{}},{"Derived",{},"getter-not-evaluated"}};
    };
    const auto snapshot = RuntimeValueSnapshot::capture(object,{},properties);
    require(snapshot.size() == 4,"fixture includes private nodes internally");
    const auto json = Json::parse(runtimePublicSnapshotJson(snapshot));
    require(json.at("schema") == "mparser.value-snapshot" && json.at("version") == 1,"versioned projection");
    const auto text = json.dump();
    require(text.find("secret") == std::string::npos && text.find("hidden") == std::string::npos &&
        text.find("91234567") == std::string::npos && text.find("81234567") == std::string::npos,
        "private names and values never reach wire data");
    require(json.at("nodes").size() == 2,"unreachable private nodes and their count are not exposed");
    const auto& root = json.at("nodes").at(0);
    require(root.at("properties").at(0).at("name") == "PublicValue" &&
        root.at("properties").at(1).at("value") == 0 &&
        root.at("properties").at(2).at("unavailable") == "getter-not-evaluated",
        "public storage, cycles and unevaluated getters have distinct representations");
    (*object.sharedFields)["public"] = makeRuntimeNumberValue(99);
    object.sharedFields->erase("self"); object = {};
    require(Json::parse(runtimePublicSnapshotJson(snapshot)) == json,"projection freezes values and access metadata");
    const auto opaque = project(makeRuntimeObjectScalar("Foreign",{{"secret",makeRuntimeNumberValue(42)}},true));
    require(opaque.at("nodes").size() == 1 && opaque.at("nodes").at(0).at("representation") == "opaque",
        "unknown class metadata never grants permission to read fields");
    RuntimeFunctionHandle callable;
    callable.kind = RuntimeFunctionHandleKind::Anonymous;
    callable.display = "@(x) x+secret";
    callable.parameters = {"x"};
    callable.capturedVariables = {{"secret",makeRuntimeNumberValue(71234567)}};
    callable.receiver = makeRuntimeObjectScalar("Receiver",{{"private",makeRuntimeNumberValue(61234567)}},true);
    const auto closure = project(makeRuntimeFunctionHandleValue(std::move(callable)));
    require(closure.at("nodes").size() == 1 && closure.dump().find("secret") == std::string::npos &&
        closure.dump().find("1234567") == std::string::npos,"closure captures and receiver graph are redacted");
}
void precisionAndContainers() {
    for (const auto bits : {0x8000000000000000ULL,0x7ff8000000001234ULL,0x7ff0000000000000ULL}) {
        const auto value = makeRuntimeNumberValue(std::bit_cast<double>(bits));
        const auto encoded = project(value).at("nodes").at(0).at("numeric").at("real_bits").at(0).get<std::string>();
        require(std::stoull(encoded,nullptr,16) == bits,"binary64 bits survive JSON without numeric rounding");
    }
    RuntimeNumericElementValue integer;
    integer.numericClass = RuntimeNumericClass::UInt64;
    integer.integerRealBits = std::numeric_limits<std::uint64_t>::max();
    const auto exact = runtimeNumericValueFromElements({1,1},{integer},RuntimeNumericClass::UInt64);
    require(exact.has_value(),"exact integer fixture");
    require(project(*exact).at("nodes").at(0).at("numeric").at("integer_real_bits").at(0) == "ffffffffffffffff",
        "uint64 values retain all bits");
    const auto container = project(makeRuntimeCellValue({makeRuntimeStructValue({{"value",makeRuntimeNumberValue(3)}})}));
    require(container.at("nodes").size() == 3 && container.at("nodes").at(0).at("elements").at(0) == 1,
        "ordinary data remains structured and IDs are local and remapped");
}
}
int main() {
    try { privacy(); precisionAndContainers(); std::cout << "public snapshot privacy and exact read-only projection passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
