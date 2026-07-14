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
    Object,
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
    std::shared_ptr<std::map<std::string, RuntimeValue>> sharedFields;
    bool handleObject = false;
    size_t opaqueId = 0;
    size_t rows = 0;
    size_t columns = 0;
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

std::string runtimeValueToString(const RuntimeValue& value);

} // namespace mparser
