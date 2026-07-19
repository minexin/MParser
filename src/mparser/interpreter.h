#pragma once

#include "mparser/diagnostic.h"
#include "mparser/semantic.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mparser {

enum class RuntimeValueKind {
    Missing,
    Number,
    String,
    Vector,
    Matrix,
    Cell,
    FunctionHandle,
    Struct,
    CommaSeparatedList,
    NameValueArgument,
    Object,
};

enum class RuntimeNumericClass {
    Double,
    Logical,
};

struct RuntimeValue {
    RuntimeValueKind kind = RuntimeValueKind::Missing;
    double number = 0.0;
    std::string text;
    std::vector<double> elements;
    std::vector<RuntimeValue> cells;
    std::string className;
    std::string enumerationMemberName;
    std::map<std::string, RuntimeValue> fields;
    std::vector<std::map<std::string, RuntimeValue>> structElements;
    std::vector<std::string> fieldOrder;
    std::shared_ptr<std::map<std::string, RuntimeValue>> sharedFields;
    bool handleObject = false;
    size_t opaqueId = 0;
    size_t rows = 0;
    size_t columns = 0;
    std::vector<size_t> dimensions;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
};

struct RuntimeVariable {
    std::string name;
    RuntimeValue value;
};

struct InterpreterResult {
    std::vector<RuntimeVariable> variables;
    std::vector<Diagnostic> diagnostics;
};

class Interpreter {
public:
    InterpreterResult run(const SemanticResult& semantic);
};

RuntimeValue makeRuntimeStructValue(
    std::map<std::string, RuntimeValue> fields = {});
RuntimeValue makeRuntimeNameValueArgument(std::string name,
                                          RuntimeValue value);
std::string runtimeValueToString(const RuntimeValue& value);

} // namespace mparser
