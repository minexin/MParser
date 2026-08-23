#include "mparser/runtime/core/value/runtime_value.h"

#include "mparser/runtime/core/object_model/runtime_metadata.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace mparser {
namespace {

std::atomic_size_t nextCallableContextIdentity{1};
std::atomic_size_t nextFunctionHandleIdentity{1};

class RuntimeValueContractValidator {
public:
    RuntimeValueContractResult validate(const RuntimeValue& value) {
        if (validateValue(value, "$")) {
            return RuntimeValueContractResult{true, {}, {}};
        }
        return RuntimeValueContractResult{
            false, std::move(errorPath_), std::move(error_)};
    }

private:
    bool fail(std::string path, std::string error) {
        if (error_.empty()) {
            errorPath_ = std::move(path);
            error_ = std::move(error);
        }
        return false;
    }

    bool validateShape(const RuntimeValue& value,
                       std::string_view path, size_t& count) {
        const auto dimensions = runtimeDimensions(value);
        const auto product = checkedRuntimeDimensionProduct(dimensions);
        if (!product) {
            return fail(std::string(path),
                        "dimension product overflows size_t");
        }
        count = *product;

        if (!value.dimensions.empty()) {
            if (value.dimensions !=
                normalizeRuntimeDimensions(value.dimensions)) {
                return fail(std::string(path),
                            "dimensions are not normalized");
            }
            if (value.rows != dimensions[0] ||
                value.columns != dimensions[1]) {
                return fail(std::string(path),
                            "rows and columns do not match dimensions");
            }
        }
        return true;
    }

    bool validateWorkspace(const RuntimeWorkspace& workspace,
                           const std::string& path) {
        for (const auto& [name, value] : workspace) {
            if (!validateValue(value, path + "." + name)) {
                return false;
            }
        }
        return true;
    }

    bool validateFunctionHandle(const RuntimeValue& value,
                                const std::string& path,
                                size_t count) {
        if (count != 1) {
            return fail(path, "function handle must be scalar");
        }
        if (!value.functionHandle) {
            return fail(path, "function handle descriptor is missing");
        }
        const RuntimeFunctionHandle& handle = *value.functionHandle;
        if (handle.identity == 0 || value.opaqueId != handle.identity) {
            return fail(path, "function handle identity is inconsistent");
        }
        if (handle.backend != RuntimeFunctionHandleBackend::Independent &&
            !handle.context) {
            return fail(path,
                        "module-bound function handle has no callable context");
        }

        const void* identity = value.functionHandle.get();
        if (!visitedFunctionHandles_.insert(identity).second) {
            return true;
        }
        if (handle.receiver &&
            !validateValue(*handle.receiver, path + ".receiver")) {
            return false;
        }
        return validateWorkspace(handle.capturedVariables,
                                 path + ".workspace");
    }

    bool validateStruct(const RuntimeValue& value,
                        const std::string& path, size_t count) {
        if (value.structElements.size() != count) {
            return fail(path,
                        "structure element count does not match shape");
        }
        std::set<std::string> fieldNames;
        for (const std::string& name : value.fieldOrder) {
            if (name.empty() || !fieldNames.insert(name).second) {
                return fail(path,
                            "structure field order is empty or duplicated");
            }
        }
        for (size_t index = 0; index < value.structElements.size();
             ++index) {
            const auto& element = value.structElements[index];
            if (element.size() != fieldNames.size()) {
                return fail(path + "[" + std::to_string(index) + "]",
                            "structure element schema does not match field order");
            }
            for (const std::string& name : value.fieldOrder) {
                const auto field = element.find(name);
                if (field == element.end()) {
                    return fail(path + "[" + std::to_string(index) + "]",
                                "structure element is missing field " + name);
                }
                if (!validateValue(field->second,
                                   path + "." + name + "[" +
                                       std::to_string(index) + "]")) {
                    return false;
                }
            }
        }
        return true;
    }

    bool validateObjectFields(const RuntimeValue& value,
                              const std::string& path) {
        const RuntimeWorkspace* fields = nullptr;
        if (value.handleObject) {
            if (!value.sharedFields || !value.fields.empty()) {
                return fail(path,
                            "handle object must use shared field storage");
            }
            const void* identity = value.sharedFields.get();
            if (!visitedSharedFields_.insert(identity).second) {
                return true;
            }
            fields = value.sharedFields.get();
        } else {
            if (value.sharedFields) {
                return fail(path,
                            "value object cannot use shared field storage");
            }
            fields = &value.fields;
        }
        return validateWorkspace(*fields, path + ".properties");
    }

    bool validateObject(const RuntimeValue& value,
                        const std::string& path, size_t count) {
        if (value.className.empty()) {
            return fail(path, "object class name is empty");
        }
        if (!value.objectElements.empty()) {
            if (value.objectElements.size() != count) {
                return fail(path,
                            "object element count does not match shape");
            }
            for (size_t index = 0; index < value.objectElements.size();
                 ++index) {
                const RuntimeValue& element = value.objectElements[index];
                if (element.kind != RuntimeValueKind::Object ||
                    !element.objectElements.empty() ||
                    runtimeShapeElementCount(element) != 1) {
                    return fail(path + "[" + std::to_string(index) + "]",
                                "object array contains a nonscalar element");
                }
                if (element.handleObject != value.handleObject) {
                    return fail(path + "[" + std::to_string(index) + "]",
                                "object array mixes value and handle elements");
                }
                if (!validateValue(element,
                                   path + "[" + std::to_string(index) + "]")) {
                    return false;
                }
            }
            return true;
        }
        if (count == 0) {
            return true;
        }
        if (count != 1) {
            return fail(path,
                        "nonscalar object has no element storage");
        }
        return validateObjectFields(value, path);
    }

    bool validateNumeric(const RuntimeValue& value,
                         const std::string& path, size_t count) {
        if (value.kind == RuntimeValueKind::Number) {
            if (count != 1) {
                return fail(path, "number must be scalar");
            }
            if (!value.elements.empty()) {
                return fail(path,
                            "scalar numeric value has array real storage");
            }
        } else if (value.elements.size() != count) {
            return fail(path,
                        "numeric payload count does not match shape");
        }

        const bool integer =
            runtimeNumericClassIsInteger(value.numericClass);
        if (integer) {
            if (value.exactIntegerElements.size() != count) {
                return fail(path,
                            "integer payload count does not match shape");
            }
            if (!value.imaginaryElements.empty()) {
                return fail(path,
                            "integer value uses floating imaginary storage");
            }
        } else if (!value.exactIntegerElements.empty() ||
                   !value.exactIntegerImaginaryElements.empty()) {
            return fail(path,
                        "noninteger value uses exact integer storage");
        }

        if (!value.numericComplex) {
            if (!value.imaginaryElements.empty() ||
                !value.exactIntegerImaginaryElements.empty()) {
                return fail(path,
                            "real numeric value has imaginary storage");
            }
            return true;
        }

        if (integer) {
            return value.exactIntegerImaginaryElements.size() == count ||
                   fail(path,
                        "complex integer imaginary payload count does not match shape");
        }
        return value.imaginaryElements.size() == count ||
               fail(path,
                    "complex imaginary payload count does not match shape");
    }

    bool validateValue(const RuntimeValue& value,
                       const std::string& path) {
        size_t count = 0;
        if (!validateShape(value, path, count)) {
            return false;
        }

        switch (value.kind) {
        case RuntimeValueKind::Missing:
            return count == 0 ||
                   fail(path, "missing value must have empty shape");
        case RuntimeValueKind::MissingArray:
            return true;
        case RuntimeValueKind::Number:
            return validateNumeric(value, path, count);
        case RuntimeValueKind::CharacterArray:
            return value.characterElements.size() == count ||
                   fail(path,
                        "character payload count does not match shape");
        case RuntimeValueKind::StringArray:
            return value.stringElements.size() == count ||
                   fail(path, "string payload count does not match shape");
        case RuntimeValueKind::Vector:
        case RuntimeValueKind::Matrix:
            return validateNumeric(value, path, count);
        case RuntimeValueKind::Cell:
        case RuntimeValueKind::CommaSeparatedList:
            if (value.cells.size() != count) {
                return fail(path,
                            "element payload count does not match shape");
            }
            for (size_t index = 0; index < value.cells.size(); ++index) {
                if (!validateValue(value.cells[index],
                                   path + "{" + std::to_string(index) + "}")) {
                    return false;
                }
            }
            return true;
        case RuntimeValueKind::FunctionHandle:
            return validateFunctionHandle(value, path, count);
        case RuntimeValueKind::Struct:
            return validateStruct(value, path, count);
        case RuntimeValueKind::NameValueArgument:
            if (count != 1 || value.text.empty() ||
                value.cells.size() != 1) {
                return fail(path,
                            "name-value argument must have one named value");
            }
            return validateValue(value.cells.front(), path + ".value");
        case RuntimeValueKind::Object:
            return validateObject(value, path, count);
        }
        return fail(path, "unknown runtime value kind");
    }

    std::set<const void*> visitedFunctionHandles_;
    std::set<const void*> visitedSharedFields_;
    std::string errorPath_;
    std::string error_;
};

class RuntimeValueArrayByteCounter {
public:
    std::optional<size_t> count(const RuntimeValue& value) {
        if (!countValue(value)) {
            return std::nullopt;
        }
        return bytes_;
    }

private:
    bool add(size_t bytes) {
        if (bytes >
            std::numeric_limits<size_t>::max() - bytes_) {
            return false;
        }
        bytes_ += bytes;
        return true;
    }

    bool addElements(size_t count, size_t elementSize) {
        if (elementSize != 0 &&
            count > std::numeric_limits<size_t>::max() /
                        elementSize) {
            return false;
        }
        return add(count * elementSize);
    }

    bool countWorkspace(const RuntimeWorkspace& workspace) {
        for (const auto& [name, value] : workspace) {
            (void)name;
            if (!countValue(value)) {
                return false;
            }
        }
        return true;
    }

    bool countFunctionHandle(const RuntimeValue& value) {
        if (!value.functionHandle) {
            return true;
        }
        const void* identity = value.functionHandle.get();
        if (!visitedFunctionHandles_.insert(identity).second) {
            return true;
        }
        const auto& handle = *value.functionHandle;
        if (handle.receiver && !countValue(*handle.receiver)) {
            return false;
        }
        return countWorkspace(handle.capturedVariables);
    }

    bool countObjectFields(const RuntimeValue& value) {
        if (value.handleObject) {
            if (!value.sharedFields) {
                return true;
            }
            const void* identity = value.sharedFields.get();
            if (!visitedSharedFields_.insert(identity).second) {
                return true;
            }
            return countWorkspace(*value.sharedFields);
        }
        return countWorkspace(value.fields);
    }

    bool countNumeric(const RuntimeValue& value) {
        const size_t realCount =
            value.kind == RuntimeValueKind::Number
                ? size_t{1}
                : value.elements.size();
        return addElements(realCount, sizeof(double)) &&
               addElements(value.imaginaryElements.size(),
                           sizeof(double)) &&
               addElements(value.exactIntegerElements.size(),
                           sizeof(std::uint64_t)) &&
               addElements(value.exactIntegerImaginaryElements.size(),
                           sizeof(std::uint64_t));
    }

    bool countValue(const RuntimeValue& value) {
        switch (value.kind) {
        case RuntimeValueKind::Missing:
        case RuntimeValueKind::MissingArray:
            return true;
        case RuntimeValueKind::Number:
            return countNumeric(value);
        case RuntimeValueKind::CharacterArray:
            return addElements(value.characterElements.size(),
                               sizeof(char16_t));
        case RuntimeValueKind::StringArray:
            for (const auto& element : value.stringElements) {
                if (!addElements(element.value.size(),
                                 sizeof(char16_t))) {
                    return false;
                }
            }
            return true;
        case RuntimeValueKind::Vector:
        case RuntimeValueKind::Matrix:
            return countNumeric(value);
        case RuntimeValueKind::Cell:
        case RuntimeValueKind::CommaSeparatedList:
            for (const auto& element : value.cells) {
                if (!countValue(element)) {
                    return false;
                }
            }
            return true;
        case RuntimeValueKind::FunctionHandle:
            return countFunctionHandle(value);
        case RuntimeValueKind::Struct:
            for (const auto& element : value.structElements) {
                if (!countWorkspace(element)) {
                    return false;
                }
            }
            return true;
        case RuntimeValueKind::NameValueArgument:
            for (const auto& element : value.cells) {
                if (!countValue(element)) {
                    return false;
                }
            }
            return true;
        case RuntimeValueKind::Object:
            if (!value.objectElements.empty()) {
                for (const auto& element : value.objectElements) {
                    if (!countValue(element)) {
                        return false;
                    }
                }
                return true;
            }
            return countObjectFields(value);
        }
        return false;
    }

    size_t bytes_ = 0;
    std::set<const void*> visitedFunctionHandles_;
    std::set<const void*> visitedSharedFields_;
};

} // namespace

RuntimeValue makeRuntimeMissingValue() {
    return RuntimeValue{};
}

RuntimeValue makeRuntimeMissingArrayValue(
    std::vector<size_t> dimensions) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::MissingArray;
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

RuntimeValue makeRuntimeNumberValue(
    double value, RuntimeNumericClass numericClass) {
    if (auto result = runtimeNumericValueFromLogicalOrder(
            {1, 1}, {value}, numericClass)) {
        return std::move(*result);
    }
    RuntimeValue result;
    result.kind = RuntimeValueKind::Number;
    result.number = value;
    result.numericClass = numericClass;
    setRuntimeDimensions(result, {1, 1});
    return result;
}

RuntimeValue makeRuntimeLogicalValue(bool value) {
    return makeRuntimeNumberValue(value ? 1.0 : 0.0,
                                  RuntimeNumericClass::Logical);
}

RuntimeValue makeRuntimeVectorValue(
    std::vector<double> values,
    RuntimeNumericClass numericClass) {
    if (auto result = runtimeNumericValueFromLogicalOrder(
            {1, values.size()}, values, numericClass)) {
        return std::move(*result);
    }
    RuntimeValue result;
    result.kind = RuntimeValueKind::Vector;
    result.elements = std::move(values);
    result.numericClass = numericClass;
    setRuntimeDimensions(result, {1, result.elements.size()});
    return result;
}

RuntimeValue makeRuntimeMatrixValue(
    size_t rows, size_t columns, std::vector<double> values,
    RuntimeNumericClass numericClass) {
    std::vector<double> logicalValues;
    if (rows == 0 || columns <=
                         std::numeric_limits<size_t>::max() / rows) {
        const size_t count = rows * columns;
        if (count == values.size()) {
            logicalValues.reserve(count);
            for (size_t column = 0; column < columns; ++column) {
                for (size_t row = 0; row < rows; ++row) {
                    logicalValues.push_back(
                        values[row * columns + column]);
                }
            }
            if (auto result = runtimeNumericValueFromLogicalOrder(
                    {rows, columns}, std::move(logicalValues),
                    numericClass)) {
                return std::move(*result);
            }
        }
    }
    RuntimeValue result;
    result.kind = RuntimeValueKind::Matrix;
    result.elements = std::move(values);
    result.numericClass = numericClass;
    setRuntimeDimensions(result, {rows, columns});
    return result;
}

RuntimeValue makeRuntimeCellValue(std::vector<RuntimeValue> values) {
    const size_t elementCount = values.size();
    return makeRuntimeCellValue({1, elementCount}, std::move(values));
}

RuntimeValue makeRuntimeCellValue(
    std::vector<size_t> dimensions, std::vector<RuntimeValue> values) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Cell;
    result.cells = std::move(values);
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

RuntimeValue makeRuntimeStructValue(RuntimeWorkspace fields) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Struct;
    result.fieldOrder.reserve(fields.size());
    for (const auto& [name, value] : fields) {
        (void)value;
        result.fieldOrder.push_back(name);
    }
    result.structElements.push_back(std::move(fields));
    setRuntimeDimensions(result, {1, 1});
    return result;
}

RuntimeValue makeRuntimeNameValueArgument(std::string name,
                                          RuntimeValue value) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::NameValueArgument;
    result.text = std::move(name);
    result.cells.push_back(std::move(value));
    setRuntimeDimensions(result, {1, 1});
    return result;
}

std::shared_ptr<RuntimeCallableContext> makeRuntimeCallableContext() {
    auto context = std::make_shared<RuntimeCallableContext>();
    context->identity = nextCallableContextIdentity.fetch_add(
        1, std::memory_order_relaxed);
    return context;
}

RuntimeValue makeRuntimeFunctionHandleValue(
    RuntimeFunctionHandle handle) {
    handle.identity = nextFunctionHandleIdentity.fetch_add(
        1, std::memory_order_relaxed);
    auto descriptor =
        std::make_shared<RuntimeFunctionHandle>(std::move(handle));

    RuntimeValue result;
    result.kind = RuntimeValueKind::FunctionHandle;
    result.functionHandle = std::move(descriptor);
    result.opaqueId = result.functionHandle->identity;
    result.text = result.functionHandle->display;
    setRuntimeDimensions(result, {1, 1});
    return result;
}

RuntimeWorkspace captureRuntimeWorkspace(
    const RuntimeWorkspace& workspace,
    const std::vector<std::string>& captureNames) {
    RuntimeWorkspace captured;
    for (const auto& name : captureNames) {
        const auto value = workspace.find(name);
        if (value != workspace.end()) {
            captured.emplace(name, value->second);
        }
    }
    return captured;
}

std::string_view runtimeValueKindName(RuntimeValueKind kind) {
    switch (kind) {
    case RuntimeValueKind::Missing:
    case RuntimeValueKind::MissingArray:
        return "missing";
    case RuntimeValueKind::Number:
        return "number";
    case RuntimeValueKind::CharacterArray:
        return "character-array";
    case RuntimeValueKind::StringArray:
        return "string-array";
    case RuntimeValueKind::Vector:
        return "vector";
    case RuntimeValueKind::Matrix:
        return "matrix";
    case RuntimeValueKind::Cell:
        return "cell";
    case RuntimeValueKind::FunctionHandle:
        return "function-handle";
    case RuntimeValueKind::Struct:
        return "struct";
    case RuntimeValueKind::CommaSeparatedList:
        return "comma-separated-list";
    case RuntimeValueKind::NameValueArgument:
        return "name-value-argument";
    case RuntimeValueKind::Object:
        return "object";
    }
    return "missing";
}

std::string_view runtimeValueOwnershipName(
    RuntimeValueOwnership ownership) {
    switch (ownership) {
    case RuntimeValueOwnership::Immediate:
        return "immediate";
    case RuntimeValueOwnership::Value:
        return "value";
    case RuntimeValueOwnership::SharedHandle:
        return "shared-handle";
    case RuntimeValueOwnership::Callable:
        return "callable";
    case RuntimeValueOwnership::Transient:
        return "transient";
    }
    return "transient";
}

RuntimeValueOwnership runtimeValueOwnership(const RuntimeValue& value) {
    switch (value.kind) {
    case RuntimeValueKind::Number:
        return RuntimeValueOwnership::Immediate;
    case RuntimeValueKind::FunctionHandle:
        return RuntimeValueOwnership::Callable;
    case RuntimeValueKind::Object:
        return value.handleObject
                   ? RuntimeValueOwnership::SharedHandle
                   : RuntimeValueOwnership::Value;
    case RuntimeValueKind::Missing:
    case RuntimeValueKind::CommaSeparatedList:
    case RuntimeValueKind::NameValueArgument:
        return RuntimeValueOwnership::Transient;
    case RuntimeValueKind::MissingArray:
        return RuntimeValueOwnership::Value;
    case RuntimeValueKind::CharacterArray:
    case RuntimeValueKind::StringArray:
    case RuntimeValueKind::Vector:
    case RuntimeValueKind::Matrix:
    case RuntimeValueKind::Cell:
    case RuntimeValueKind::Struct:
        return RuntimeValueOwnership::Value;
    }
    return RuntimeValueOwnership::Transient;
}

bool runtimeValueIsStorable(const RuntimeValue& value) {
    return runtimeValueOwnership(value) !=
           RuntimeValueOwnership::Transient;
}

RuntimeValueContractResult validateRuntimeValueContract(
    const RuntimeValue& value) {
    RuntimeValueContractValidator validator;
    return validator.validate(value);
}

std::optional<size_t> runtimeValueArrayBytes(
    const RuntimeValue& value) {
    RuntimeValueArrayByteCounter counter;
    return counter.count(value);
}

std::string runtimeFunctionHandleText(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::FunctionHandle ||
        !value.functionHandle) {
        return {};
    }

    const RuntimeFunctionHandle& handle = *value.functionHandle;
    if (handle.kind == RuntimeFunctionHandleKind::Anonymous) {
        return handle.display;
    }
    if (!handle.display.empty()) {
        return handle.display.front() == '@'
                   ? handle.display.substr(1)
                   : handle.display;
    }
    if (handle.kind == RuntimeFunctionHandleKind::Method) {
        return handle.className.empty()
                   ? handle.methodName
                   : handle.className + "." + handle.methodName;
    }
    return handle.targetName;
}

RuntimeValue runtimeFunctionHandleMetadata(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::FunctionHandle ||
        !value.functionHandle) {
        return makeRuntimeStructValue();
    }

    const RuntimeFunctionHandle& handle = *value.functionHandle;
    RuntimeWorkspace fields{
        {"file", makeRuntimeCharacterVectorUtf8(handle.sourceFile)},
        {"function",
         makeRuntimeCharacterVectorUtf8(runtimeFunctionHandleText(value))},
        {"type", makeRuntimeCharacterVectorUtf8(
                     handle.kind == RuntimeFunctionHandleKind::Anonymous
                         ? "anonymous"
                         : "simple")},
    };
    if (handle.kind == RuntimeFunctionHandleKind::Anonymous) {
        fields["workspace"] = makeRuntimeCellValue(
            {makeRuntimeStructValue(handle.capturedVariables)});
    }
    RuntimeValue result = makeRuntimeStructValue(std::move(fields));
    result.fieldOrder = {"function", "type", "file"};
    if (handle.kind == RuntimeFunctionHandleKind::Anonymous) {
        result.fieldOrder.push_back("workspace");
    }
    return result;
}

void writeRuntimeNumericComponent(
    std::ostringstream& output,
    const RuntimeNumericElementValue& element, bool imaginary,
    bool magnitude) {
    if (runtimeNumericClassIsInteger(element.numericClass)) {
        const std::uint64_t bits =
            imaginary ? element.integerImaginaryBits
                      : element.integerRealBits;
        if (runtimeNumericClassIsSignedInteger(element.numericClass)) {
            const std::int64_t value =
                std::bit_cast<std::int64_t>(bits);
            if (magnitude && value < 0) {
                output << (std::uint64_t{0} -
                           static_cast<std::uint64_t>(value));
            } else {
                output << value;
            }
            return;
        }
        output << bits;
        return;
    }

    const double value = imaginary ? element.imaginary : element.real;
    output << (magnitude ? std::fabs(value) : value);
}

void writeRuntimeNumericElement(std::ostringstream& output,
                                const RuntimeValue& value,
                                size_t storageOffset) {
    const auto element =
        runtimeNumericStorageElementValue(value, storageOffset);
    if (!element) {
        output << "<invalid-numeric>";
        return;
    }

    writeRuntimeNumericComponent(output, *element, false, false);
    if (!element->complex) {
        return;
    }

    bool negative = std::signbit(element->imaginary);
    if (runtimeNumericClassIsSignedInteger(element->numericClass)) {
        negative = std::bit_cast<std::int64_t>(
                       element->integerImaginaryBits) < 0;
    }
    output << (negative ? '-' : '+');
    writeRuntimeNumericComponent(output, *element, true, negative);
    output << 'i';
}

std::string runtimeValueToString(const RuntimeValue& value) {
    std::ostringstream output;
    output << std::setprecision(15);

    switch (value.kind) {
    case RuntimeValueKind::Missing:
        return "<missing>";
    case RuntimeValueKind::MissingArray: {
        const auto dimensions = runtimeDimensions(value);
        if (dimensions == std::vector<size_t>{1, 1}) {
            return "<missing>";
        }
        output << "missing(";
        for (size_t index = 0; index < dimensions.size(); ++index) {
            if (index != 0) {
                output << 'x';
            }
            output << dimensions[index];
        }
        output << ')';
        return output.str();
    }
    case RuntimeValueKind::Number:
        writeRuntimeNumericElement(output, value, 0);
        return output.str();
    case RuntimeValueKind::CharacterArray: {
        const auto dimensions = runtimeDimensions(value);
        if (dimensions == std::vector<size_t>{0, 0}) {
            return "''";
        }
        if (dimensions.size() == 2 && dimensions[0] == 1) {
            const std::string text =
                runtimeUtf16ToUtf8(value.characterElements);
            output << '\'';
            for (const char character : text) {
                output << character;
                if (character == '\'') {
                    output << '\'';
                }
            }
            output << '\'';
            return output.str();
        }
        output << "char(" << dimensions[0] << "x" << dimensions[1]
               << ")[";
        for (size_t row = 0; row < dimensions[0]; ++row) {
            if (row != 0) {
                output << "; ";
            }
            const std::u16string_view rowText =
                dimensions[1] == 0
                    ? std::u16string_view{}
                    : std::u16string_view(
                          value.characterElements.data() +
                              row * dimensions[1],
                          dimensions[1]);
            output << '\'' << runtimeUtf16ToUtf8(rowText) << '\'';
        }
        output << "]";
        return output.str();
    }
    case RuntimeValueKind::StringArray: {
        if (isRuntimeStringScalar(value)) {
            if (value.stringElements.front().missing) {
                return "<missing>";
            }
            const std::string text =
                runtimeUtf16ToUtf8(value.stringElements.front().value);
            output << '"';
            for (const char character : text) {
                output << character;
                if (character == '"') {
                    output << '"';
                }
            }
            output << '"';
            return output.str();
        }
        const auto dimensions = runtimeDimensions(value);
        output << "string(";
        for (size_t index = 0; index < dimensions.size(); ++index) {
            if (index != 0) {
                output << "x";
            }
            output << dimensions[index];
        }
        output << ")[";
        for (size_t index = 0; index < value.stringElements.size();
             ++index) {
            if (index != 0) {
                output << ", ";
            }
            if (value.stringElements[index].missing) {
                output << "<missing>";
            } else {
                output << '"'
                       << runtimeUtf16ToUtf8(
                              value.stringElements[index].value)
                       << '"';
            }
        }
        output << "]";
        return output.str();
    }
    case RuntimeValueKind::Vector:
        output << "[";
        for (size_t index = 0; index < value.elements.size(); ++index) {
            if (index > 0) {
                output << " ";
            }
            writeRuntimeNumericElement(output, value, index);
        }
        output << "]";
        return output.str();
    case RuntimeValueKind::Matrix:
        if (runtimeDimensionCount(value) > 2) {
            const auto dimensions = runtimeDimensions(value);
            output << "array(";
            for (size_t index = 0; index < dimensions.size(); ++index) {
                if (index != 0) {
                    output << "x";
                }
                output << dimensions[index];
            }
            output << ")[";
            for (size_t index = 0; index < value.elements.size();
                 ++index) {
                if (index != 0) {
                    output << " ";
                }
                const auto storageOffset =
                    runtimeColumnMajorLinearToStorageOffset(value, index);
                writeRuntimeNumericElement(
                    output, value, *storageOffset);
            }
            output << "]";
            return output.str();
        }
        output << "[";
        for (size_t row = 0; row < value.rows; ++row) {
            if (row > 0) {
                output << "; ";
            }
            for (size_t column = 0; column < value.columns; ++column) {
                if (column > 0) {
                    output << " ";
                }
                writeRuntimeNumericElement(
                    output, value,
                    row * value.columns + column);
            }
        }
        output << "]";
        return output.str();
    case RuntimeValueKind::Cell:
        if (runtimeDimensionCount(value) > 2) {
            const auto dimensions = runtimeDimensions(value);
            output << "cell(";
            for (size_t index = 0; index < dimensions.size(); ++index) {
                if (index != 0) {
                    output << "x";
                }
                output << dimensions[index];
            }
            output << "){";
            for (size_t index = 0; index < value.cells.size(); ++index) {
                if (index != 0) {
                    output << ", ";
                }
                const auto storageOffset =
                    runtimeColumnMajorLinearToStorageOffset(value, index);
                output << runtimeValueToString(
                    value.cells[*storageOffset]);
            }
            output << "}";
            return output.str();
        }
        output << "{";
        for (size_t index = 0; index < value.cells.size(); ++index) {
            if (index > 0) {
                output << ", ";
            }
            output << runtimeValueToString(value.cells[index]);
        }
        output << "}";
        return output.str();
    case RuntimeValueKind::CommaSeparatedList:
        output << "<comma-separated-list:" << value.cells.size()
               << ">";
        return output.str();
    case RuntimeValueKind::FunctionHandle:
        return value.text.empty() ? "<function_handle>" : value.text;
    case RuntimeValueKind::Struct: {
        const size_t elementCount = runtimeStructElementCount(value);
        if (elementCount != 1) {
            const auto dimensions = runtimeDimensions(value);
            output << "struct(";
            for (size_t index = 0; index < dimensions.size(); ++index) {
                if (index != 0) {
                    output << "x";
                }
                output << dimensions[index];
            }
            output << "; fields=";
            const auto fieldNames = runtimeStructFieldOrder(value);
            for (size_t index = 0; index < fieldNames.size(); ++index) {
                if (index != 0) {
                    output << ",";
                }
                output << fieldNames[index];
            }
            output << ")";
            return output.str();
        }
        output << "struct(";
        const auto fieldNames = runtimeStructFieldOrder(value);
        for (size_t index = 0; index < fieldNames.size(); ++index) {
            if (index != 0) {
                output << ", ";
            }
            const RuntimeValue* field =
                runtimeStructField(value, fieldNames[index]);
            output << fieldNames[index] << "="
                   << (field ? runtimeValueToString(*field)
                             : std::string("<missing>"));
        }
        output << ")";
        return output.str();
    }
    case RuntimeValueKind::NameValueArgument:
        return value.text + "=" +
               (value.cells.empty()
                    ? std::string("<missing>")
                    : runtimeValueToString(value.cells.front()));
    case RuntimeValueKind::Object:
        if (isRuntimeMetadataObject(value)) {
            output << "<"
                   << canonicalRuntimeMetadataClassName(value.className);
            if (isRuntimeMetadataScalar(value)) {
                output << " " << value.text;
            } else {
                const auto dimensions = runtimeDimensions(value);
                output << " ";
                for (size_t index = 0; index < dimensions.size();
                     ++index) {
                    if (index != 0) {
                        output << "x";
                    }
                    output << dimensions[index];
                }
            }
            output << ">";
            return output.str();
        }
        output << "<" << value.className;
        if (!value.enumerationMemberName.empty()) {
            output << "." << value.enumerationMemberName;
        } else if (!isRuntimeScalarObject(value)) {
            const auto dimensions = runtimeDimensions(value);
            output << " ";
            for (size_t index = 0; index < dimensions.size(); ++index) {
                if (index != 0) {
                    output << "x";
                }
                output << dimensions[index];
            }
        }
        output << ">";
        return output.str();
    }
    return "<missing>";
}

} // namespace mparser
