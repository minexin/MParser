#pragma once

#include "mparser/diagnostic.h"
#include "mparser/semantic.h"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
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

enum class RuntimeFunctionHandleKind {
    Anonymous,
    Function,
    Builtin,
    Method,
};

enum class RuntimeFunctionHandleBackend {
    Independent,
    Hir,
    Bytecode,
};

struct RuntimeCallableContext {
    size_t identity = 0;
};

struct RuntimeFunctionHandle;

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
    std::shared_ptr<RuntimeFunctionHandle> functionHandle;
    bool handleObject = false;
    size_t opaqueId = 0;
    size_t rows = 0;
    size_t columns = 0;
    std::vector<size_t> dimensions;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
};

struct RuntimeFunctionHandle {
    size_t identity = 0;
    RuntimeFunctionHandleKind kind = RuntimeFunctionHandleKind::Function;
    RuntimeFunctionHandleBackend backend =
        RuntimeFunctionHandleBackend::Independent;
    std::shared_ptr<RuntimeCallableContext> context;
    std::string display;
    std::string targetName;
    std::string className;
    std::string methodName;
    std::string declaringClass;
    std::string lexicalClassName;
    std::string sourceFile;
    std::optional<RuntimeValue> receiver;
    std::vector<std::string> parameters;
    std::map<std::string, RuntimeValue> capturedVariables;
    const HirNode* hirBody = nullptr;
    size_t entry = 0;
    size_t end = 0;
    SourceSpan span;
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
std::shared_ptr<RuntimeCallableContext> makeRuntimeCallableContext();
RuntimeValue makeRuntimeFunctionHandleValue(RuntimeFunctionHandle handle);
std::string runtimeFunctionHandleText(const RuntimeValue& value);
RuntimeValue runtimeFunctionHandleMetadata(const RuntimeValue& value);
std::string runtimeValueToString(const RuntimeValue& value);

} // namespace mparser
