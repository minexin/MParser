#include "mparser/runtime/core/value/runtime_value_snapshot.h"

#include "mparser/runtime/core/object_model/runtime_graphics.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/object_model/runtime_metadata.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <map>

namespace mparser {
namespace {

class SnapshotBuilder {
public:
    explicit SnapshotBuilder(RuntimeSnapshotLimits limits) : limits_(limits) {}
    std::vector<RuntimeSnapshotNode> capture(const RuntimeValue& value) {
        append(value, 0);
        const auto validation = validateRuntimeValueContract(value);
        if (!validation.valid) { invalid(validation.path + ": " + validation.error); }
        return std::move(nodes_);
    }
private:
    [[noreturn]] void invalid(std::string message) const {
        throw RuntimeSnapshotError(RuntimeSnapshotFailure::InvalidValue, std::move(message));
    }
    void bytes(size_t count, size_t width = 1) {
        if (width && count > (limits_.maximumBytes - usedBytes_) / width) {
            throw RuntimeSnapshotError(RuntimeSnapshotFailure::ByteLimit, "value snapshot exceeds its byte budget");
        }
        usedBytes_ += count * width;
    }
    std::string text(const std::string& value) { bytes(value.size()); return value; }
    std::vector<std::string> names(const std::vector<std::string>& values) {
        bytes(values.size(), sizeof(std::string));
        std::vector<std::string> result;
        result.reserve(values.size());
        for (const auto& value : values) { result.push_back(text(value)); }
        return result;
    }
    std::vector<RuntimeSnapshotField> fields(const RuntimeWorkspace& values, size_t depth) {
        bytes(values.size(), sizeof(RuntimeSnapshotField));
        std::vector<RuntimeSnapshotField> result;
        result.reserve(values.size());
        for (const auto& [name, value] : values) {
            auto key = text(name);
            const auto id = append(value, depth + 1);
            result.emplace_back(std::move(key), id);
        }
        return result;
    }
    RuntimeSnapshotId append(const RuntimeValue& value, size_t depth) {
        // Resolve aliases before applying depth/node limits; a self reference
        // is one edge, not another recursively retained object.
        if (value.kind == RuntimeValueKind::Object && value.sharedFields) {
            if (const auto found = handles_.find(value.sharedFields.get()); found != handles_.end()) {
                return found->second;
            }
        }
        if (value.functionHandle) {
            if (const auto found = callables_.find(value.functionHandle.get()); found != callables_.end()) {
                return found->second;
            }
        }
        if (value.tabularStorage) {
            if (const auto found = tables_.find(value.tabularStorage.get()); found != tables_.end()) {
                return found->second;
            }
        }
        if (depth > limits_.maximumDepth) {
            throw RuntimeSnapshotError(RuntimeSnapshotFailure::DepthLimit, "value snapshot exceeds its depth limit");
        }
        if (nodes_.size() >= limits_.maximumNodes) {
            throw RuntimeSnapshotError(RuntimeSnapshotFailure::NodeLimit, "value snapshot exceeds its node limit");
        }
        bytes(1, sizeof(RuntimeSnapshotNode));
        const auto id = nodes_.size();
        nodes_.emplace_back();
        if (value.kind == RuntimeValueKind::Object && value.sharedFields) { handles_.emplace(value.sharedFields.get(), id); }
        if (value.functionHandle) { callables_.emplace(value.functionHandle.get(), id); }
        if (value.tabularStorage) { tables_.emplace(value.tabularStorage.get(), id); }

        RuntimeSnapshotNode node;
        node.kind = value.kind;
        node.dimensions = runtimeDimensions(value);
        bytes(node.dimensions.size(), sizeof(size_t));
        const auto count = checkedRuntimeDimensionProduct(node.dimensions);
        if (!count) { invalid("value snapshot dimensions overflow"); }
        if ((value.sparseStorage && !isRuntimeSparseValue(value)) ||
            (value.categoricalStorage && !isRuntimeCategoricalValue(value)) ||
            (value.tabularStorage && !isRuntimeTabularValue(value))) {
            invalid("value snapshot has incompatible shared storage");
        }
        node.className = text(value.className);
        node.text = text(value.text);
        node.enumerationMemberName = text(value.enumerationMemberName);
        node.handleObject = value.handleObject;
        node.moduleBound = static_cast<bool>(value.objectContext);
        node.numericClass = value.numericClass;
        node.numericComplex = value.numericComplex;
        if (value.dynamicPropertyOwner) {
            node.weakOwner = RuntimeSnapshotWeakOwner{
                text(value.dynamicPropertyOwner->className), !value.dynamicPropertyOwner->fields.expired()};
        }
        switch (value.kind) {
        case RuntimeValueKind::Missing:
        case RuntimeValueKind::MissingArray:
            break;
        case RuntimeValueKind::Number:
        case RuntimeValueKind::Vector:
        case RuntimeValueKind::Matrix:
            bytes(*count, sizeof(RuntimeNumericElementValue));
            node.numeric.reserve(*count);
            for (size_t index = 0; index < *count; ++index) {
                const auto element = runtimeNumericElementValue(value, index);
                if (!element) { invalid("value snapshot has an invalid numeric element"); }
                node.numeric.push_back(*element);
            }
            break;
        case RuntimeValueKind::CharacterArray:
            bytes(*count, sizeof(char16_t));
            node.characters.reserve(*count);
            for (size_t index = 0; index < *count; ++index) {
                const auto element = runtimeCharacterElement(value, index);
                if (!element) { invalid("value snapshot has an invalid character element"); }
                node.characters.push_back(*element);
            }
            break;
        case RuntimeValueKind::StringArray:
            bytes(*count, sizeof(RuntimeStringElement));
            node.strings.reserve(*count);
            for (size_t index = 0; index < *count; ++index) {
                const auto element = runtimeStringElement(value, index);
                if (!element) { invalid("value snapshot has an invalid string element"); }
                bytes(element->value.size(), sizeof(char16_t));
                node.strings.push_back(*element);
            }
            break;
        case RuntimeValueKind::Cell:
        case RuntimeValueKind::CommaSeparatedList:
        case RuntimeValueKind::NameValueArgument:
            if (value.kind == RuntimeValueKind::NameValueArgument && value.cells.size() != 1) {
                invalid("value snapshot has no named argument value");
            }
            bytes(*count, sizeof(RuntimeSnapshotId));
            node.elements.reserve(*count);
            for (size_t index = 0; index < *count; ++index) {
                const auto offset = runtimeColumnMajorLinearToStorageOffset(value, index);
                if (!offset || *offset >= value.cells.size()) { invalid("value snapshot has an invalid cell element"); }
                node.elements.push_back(append(value.cells[*offset], depth + 1));
            }
            break;
        case RuntimeValueKind::Struct:
            node.fieldOrder = names(runtimeStructFieldOrder(value));
            bytes(*count, sizeof(std::vector<RuntimeSnapshotField>));
            node.structElements.reserve(*count);
            for (size_t index = 0; index < *count; ++index) {
                const auto offset = runtimeColumnMajorLinearToStorageOffset(value, index);
                const auto element = offset ? runtimeStructElement(value, *offset) : nullptr;
                if (!element || element->size() != node.fieldOrder.size()) { invalid("value snapshot has an invalid struct element"); }
                node.structElements.push_back(fields(*element, depth));
            }
            break;
        case RuntimeValueKind::FunctionHandle:
            if (!value.functionHandle) { invalid("value snapshot has no callable descriptor"); }
            {
                const auto& handle = *value.functionHandle;
                RuntimeSnapshotCallable callable;
                callable.kind = handle.kind;
                callable.backend = handle.backend;
                callable.display = text(handle.display);
                callable.targetName = text(handle.targetName);
                callable.className = text(handle.className);
                callable.methodName = text(handle.methodName);
                callable.declaringClass = text(handle.declaringClass);
                callable.moduleBound = static_cast<bool>(handle.context);
                callable.parameters = names(handle.parameters);
                if (handle.receiver) { callable.receiver = append(*handle.receiver, depth + 1); }
                callable.captures = fields(handle.capturedVariables, depth);
                node.callable = std::move(callable);
            }
            break;
        case RuntimeValueKind::Object:
            captureObject(value, node, depth, *count);
            break;
        default:
            invalid("value snapshot has an unknown value kind");
        }
        nodes_[id] = std::move(node);
        return id;
    }
    void captureObject(const RuntimeValue& value, RuntimeSnapshotNode& node, size_t depth, size_t count) {
        if (value.className.empty()) { invalid("value snapshot object has no class"); }
        if (isRuntimeSparseValue(value)) {
            const auto& source = *value.sparseStorage;
            std::string error;
            if (!validateRuntimeSparseStorage(value, error)) { invalid(error); }
            bytes(source.columnPointers.size(), sizeof(size_t));
            bytes(source.rowIndices.size(), sizeof(size_t));
            bytes(source.values.size(), sizeof(double));
            bytes(source.imaginaryValues.size(), sizeof(double));
            node.sparse = source;
        } else if (isRuntimeCategoricalValue(value)) {
            const auto& source = *value.categoricalStorage;
            std::string error;
            if (!validateRuntimeCategoricalStorage(value, error)) { invalid(error); }
            RuntimeCategoricalStorage categorical;
            categorical.categories = names(source.categories);
            bytes(source.codes.size(), sizeof(std::uint32_t));
            categorical.codes = source.codes;
            categorical.ordinal = source.ordinal;
            categorical.protectedCategories = source.protectedCategories;
            node.categorical = std::move(categorical);
        } else if (isRuntimeTabularValue(value)) {
            const auto& source = *value.tabularStorage;
            RuntimeSnapshotTabular table;
            table.kind = source.kind;
            table.rowCount = source.rowCount;
            table.rowAxisKind = source.rowAxisKind;
            table.rowNames = names(source.rowNames);
            table.dimensionNames = names(source.dimensionNames);
            table.description = text(source.description);
            bytes(source.variables.size(), sizeof(RuntimeSnapshotField));
            for (const auto& variable : source.variables) {
                auto name = text(variable.name);
                const auto child = append(variable.value, depth + 1);
                table.variables.emplace_back(std::move(name), child);
            }
            table.rowTimes = append(source.rowTimes, depth + 1);
            table.userData = append(source.userData, depth + 1);
            node.tabular = std::move(table);
        } else if (value.graphicsHandle) {
            // The v3 graph and reference are frozen together by the serializer.
            // The handle/graph retained while serializing is not stored here.
            node.graphicsJson = serializeRuntimeGraphicsValue(value, true);
            bytes(node.graphicsJson.size());
        } else if (isRuntimeTemporalValue(value)) {
            bytes(count, sizeof(double));
            node.temporal.reserve(count);
            for (size_t index = 0; index < count; ++index) {
                const auto payload = runtimeTemporalPayload(value, index);
                if (!payload) { invalid("value snapshot has an invalid temporal element"); }
                node.temporal.push_back(*payload);
            }
        } else if (isRuntimeMetadataArray(value)) {
            bytes(count, sizeof(RuntimeSnapshotId));
            node.elements.reserve(count);
            for (size_t index = 0; index < count; ++index) {
                const auto offset = runtimeColumnMajorLinearToStorageOffset(value, index);
                if (!offset || *offset >= value.cells.size()) { invalid("value snapshot has an invalid metadata element"); }
                node.elements.push_back(append(value.cells[*offset], depth + 1));
            }
        } else if (!value.objectElements.empty()) {
            bytes(count, sizeof(RuntimeSnapshotId));
            node.elements.reserve(count);
            for (size_t index = 0; index < count; ++index) {
                const auto element = runtimeObjectLogicalElement(value, index);
                if (!element) { invalid("value snapshot has an invalid object element"); }
                node.elements.push_back(append(*element, depth + 1));
            }
        } else if (const auto stored = runtimeObjectFields(value)) {
            node.storedFields = fields(*stored, depth);
        }
    }
    RuntimeSnapshotLimits limits_;
    size_t usedBytes_ = 0;
    std::vector<RuntimeSnapshotNode> nodes_;
    std::map<const RuntimeWorkspace*, RuntimeSnapshotId> handles_;
    std::map<const RuntimeFunctionHandle*, RuntimeSnapshotId> callables_;
    std::map<const RuntimeTabularStorage*, RuntimeSnapshotId> tables_;
};
} // namespace

RuntimeValueSnapshot RuntimeValueSnapshot::capture(const RuntimeValue& value, RuntimeSnapshotLimits limits) {
    try {
        return RuntimeValueSnapshot(SnapshotBuilder(limits).capture(value));
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const RuntimeSnapshotError&) {
        throw;
    } catch (const std::exception& error) {
        throw RuntimeSnapshotError(RuntimeSnapshotFailure::InvalidValue, error.what());
    }
}

const RuntimeSnapshotNode& RuntimeValueSnapshot::node(RuntimeSnapshotId id) const {
    if (!nodes_) { throw std::out_of_range("value snapshot is unavailable"); }
    return nodes_->at(id);
}

} // namespace mparser
