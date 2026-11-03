#include "mparser/runtime/core/value/runtime_snapshot_json.h"
#include <nlohmann/json.hpp>
#include <bit>
#include <map>

namespace mparser {
namespace {
using Json = nlohmann::json;
std::string hex(std::uint64_t value) {
    std::string result(16,'0');
    for (size_t index = 0; index < 16; ++index) {
        result[15-index] = "0123456789abcdef"[value & 15]; value >>= 4;
    }
    return result;
}
Json doubleBits(const std::vector<double>& values) {
    auto result = Json::array();
    for (const auto value : values) { result.push_back(hex(std::bit_cast<std::uint64_t>(value))); }
    return result;
}
Json codeUnits(const std::u16string& text) {
    auto result = Json::array();
    for (const auto unit : text) { result.push_back(static_cast<std::uint16_t>(unit)); }
    return result;
}
class Projection {
public:
    Projection(const RuntimeValueSnapshot& snapshot,RuntimeSnapshotLimits limits) : snapshot_(snapshot),limits_(limits) {}
    std::string render() {
        if (!snapshot_.available()) { throw std::invalid_argument("snapshot value is unavailable"); }
        append(0);
        auto text = Json{{"schema","mparser.value-snapshot"},{"version",1},{"root",0},{"nodes",std::move(nodes_)}}.dump();
        if (text.size() > limits_.maximumBytes) { throw RuntimeSnapshotError(RuntimeSnapshotFailure::ByteLimit,"public value JSON exceeds its byte limit"); }
        return text;
    }
private:
    size_t append(RuntimeSnapshotId source,size_t depth = 0) {
        if (const auto found = ids_.find(source); found != ids_.end()) { return found->second; }
        if (depth > limits_.maximumDepth) { throw RuntimeSnapshotError(RuntimeSnapshotFailure::DepthLimit,"public value exceeds its depth limit"); }
        if (nodes_.size() >= limits_.maximumNodes) { throw RuntimeSnapshotError(RuntimeSnapshotFailure::NodeLimit,"public value exceeds its node limit"); }
        const auto id = nodes_.size(); ids_.emplace(source,id); nodes_.push_back(nullptr);
        const auto& node = snapshot_.node(source);
        if (node.numeric.size() > limits_.maximumBytes / 64 || node.characters.size() > limits_.maximumBytes / 2) {
            throw RuntimeSnapshotError(RuntimeSnapshotFailure::ByteLimit,"public value payload exceeds its byte limit");
        }
        Json result{{"id",id},{"kind",runtimeValueKindName(node.kind)},
            {"class",node.className},{"dimensions",node.dimensions}};
        if (!node.numeric.empty() || node.kind == RuntimeValueKind::Number || node.kind == RuntimeValueKind::Vector ||
            node.kind == RuntimeValueKind::Matrix) {
            Json numeric{{"class",runtimeNumericClassName(node.numericClass)},{"complex",node.numericComplex},
                {"real_bits",Json::array()},{"imaginary_bits",Json::array()},
                {"integer_real_bits",Json::array()},{"integer_imaginary_bits",Json::array()}};
            for (const auto& item : node.numeric) {
                numeric["real_bits"].push_back(hex(std::bit_cast<std::uint64_t>(item.real)));
                numeric["imaginary_bits"].push_back(hex(std::bit_cast<std::uint64_t>(item.imaginary)));
                numeric["integer_real_bits"].push_back(hex(item.integerRealBits));
                numeric["integer_imaginary_bits"].push_back(hex(item.integerImaginaryBits));
            }
            result["numeric"] = std::move(numeric);
        }
        if (node.kind == RuntimeValueKind::CharacterArray) { result["code_units"] = codeUnits(node.characters); }
        if (node.kind == RuntimeValueKind::StringArray) {
            auto strings = Json::array();
            for (const auto& item : node.strings) { strings.push_back({{"missing",item.missing},{"code_units",codeUnits(item.value)}}); }
            result["strings"] = std::move(strings);
        }
        if (node.kind == RuntimeValueKind::Cell || node.kind == RuntimeValueKind::CommaSeparatedList ||
            node.kind == RuntimeValueKind::NameValueArgument || !node.elements.empty()) {
            auto elements = Json::array();
            for (const auto child : node.elements) { elements.push_back(append(child,depth+1)); }
            result["elements"] = std::move(elements);
            if (node.kind == RuntimeValueKind::NameValueArgument) { result["name"] = node.text; }
        }
        if (node.kind == RuntimeValueKind::Struct) {
            result["field_order"] = node.fieldOrder;
            auto elements = Json::array();
            for (const auto& element : node.structElements) { elements.push_back(fields(element,depth)); }
            result["struct_elements"] = std::move(elements);
        }
        if (node.sparse) {
            const auto& value = *node.sparse;
            result["sparse"] = {{"class",runtimeNumericClassName(value.numericClass)},{"complex",value.complex},
                {"column_pointers",value.columnPointers},{"row_indices",value.rowIndices},
                {"real_bits",doubleBits(value.values)},{"imaginary_bits",doubleBits(value.imaginaryValues)}};
        }
        if (node.categorical) {
            const auto& value = *node.categorical;
            result["categorical"] = {{"categories",value.categories},{"codes",value.codes},
                {"ordinal",value.ordinal},{"protected",value.protectedCategories}};
        }
        if (node.tabular) {
            const auto& table = *node.tabular;
            result["tabular"] = {{"kind",table.kind == RuntimeTabularKind::Table ? "table" : "timetable"},
                {"row_count",table.rowCount},{"row_axis_kind",static_cast<int>(table.rowAxisKind)},
                {"row_names",table.rowNames},{"dimension_names",table.dimensionNames},{"description",table.description},
                {"variables",fields(table.variables,depth)},{"row_times",append(table.rowTimes,depth+1)},{"user_data",append(table.userData,depth+1)}};
        }
        if (!node.temporal.empty()) { result["temporal_bits"] = doubleBits(node.temporal); }
        if (!node.graphicsJson.empty()) { result["graphics"] = Json::parse(node.graphicsJson); }
        if (node.callable) {
            const auto& callable = *node.callable;
            result["callable"] = {{"kind",static_cast<int>(callable.kind)},
                {"display",callable.kind == RuntimeFunctionHandleKind::Anonymous ? "<anonymous function>" : callable.display},
                {"parameters",callable.parameters},{"module_bound",callable.moduleBound},{"invocable",false}};
        }
        if (node.kind == RuntimeValueKind::Object && !node.sparse && !node.categorical && !node.tabular &&
            node.graphicsJson.empty() && node.elements.empty()) {
            result["representation"] = node.publicPropertiesKnown ? "public-properties" : "opaque";
            if (node.publicPropertiesKnown) {
                auto properties = Json::array();
                for (const auto& property : node.publicProperties) {
                    Json value{{"name",property.name}};
                    if (property.value) { value["value"] = append(*property.value,depth+1); }
                    else { value["unavailable"] = property.unavailable; }
                    properties.push_back(std::move(value));
                }
                result["properties"] = std::move(properties);
            }
            if (!node.enumerationMemberName.empty()) { result["enumeration_member"] = node.enumerationMemberName; }
        }
        if (node.weakOwner) { result["owner_alive"] = node.weakOwner->alive; }
        nodes_[id] = std::move(result);
        return id;
    }
    Json fields(const std::vector<RuntimeSnapshotField>& source,size_t depth) {
        auto result = Json::array();
        for (const auto& [name,id] : source) { result.push_back({{"name",name},{"value",append(id,depth+1)}}); }
        return result;
    }
    const RuntimeValueSnapshot& snapshot_;
    RuntimeSnapshotLimits limits_;
    std::map<RuntimeSnapshotId,size_t> ids_;
    Json nodes_ = Json::array();
};
}
std::string runtimePublicSnapshotJson(const RuntimeValueSnapshot& snapshot,RuntimeSnapshotLimits limits) { return Projection(snapshot,limits).render(); }
}
