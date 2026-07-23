#pragma once

#include "mparser/source.h"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct HirNode;

enum class RuntimeValueKind {
  Missing,
  Number,
  CharacterArray,
  StringArray,
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

enum class RuntimeValueOwnership {
  Immediate,
  Value,
  SharedHandle,
  Callable,
  Transient,
};

struct RuntimeCallableContext {
  size_t identity = 0;
  std::shared_ptr<const void> lifetimeAnchor;
};

struct RuntimeStringElement {
  std::u16string value;
  bool missing = false;

  bool operator==(const RuntimeStringElement &) const = default;
};

struct RuntimeFunctionHandle;

struct RuntimeValue {
  RuntimeValueKind kind = RuntimeValueKind::Missing;
  double number = 0.0;
  std::string text;
  std::vector<double> elements;
  std::u16string characterElements;
  std::vector<RuntimeStringElement> stringElements;
  std::vector<RuntimeValue> cells;
  std::string className;
  std::string enumerationMemberName;
  std::map<std::string, RuntimeValue> fields;
  std::vector<std::map<std::string, RuntimeValue>> structElements;
  std::vector<RuntimeValue> objectElements;
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

using RuntimeWorkspace = std::map<std::string, RuntimeValue>;

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
  RuntimeWorkspace capturedVariables;
  const HirNode *hirBody = nullptr;
  size_t entry = 0;
  size_t end = 0;
  SourceSpan span;
};

struct RuntimeVariable {
  std::string name;
  RuntimeValue value;
};

struct RuntimeValueContractResult {
  bool valid = false;
  std::string path;
  std::string error;
};

RuntimeValue makeRuntimeMissingValue();
RuntimeValue makeRuntimeNumberValue(
    double value,
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double);
RuntimeValue makeRuntimeLogicalValue(bool value);
RuntimeValue makeRuntimeVectorValue(
    std::vector<double> values,
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double);
RuntimeValue makeRuntimeMatrixValue(
    size_t rows, size_t columns, std::vector<double> values,
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double);
RuntimeValue makeRuntimeCellValue(std::vector<RuntimeValue> values);
RuntimeValue makeRuntimeCellValue(
    std::vector<size_t> dimensions, std::vector<RuntimeValue> values);
RuntimeValue makeRuntimeStructValue(RuntimeWorkspace fields = {});
RuntimeValue makeRuntimeNameValueArgument(std::string name,
                                          RuntimeValue value);
std::shared_ptr<RuntimeCallableContext> makeRuntimeCallableContext();
RuntimeValue makeRuntimeFunctionHandleValue(RuntimeFunctionHandle handle);

std::string_view runtimeValueKindName(RuntimeValueKind kind);
std::string_view runtimeValueOwnershipName(
    RuntimeValueOwnership ownership);
RuntimeValueOwnership runtimeValueOwnership(const RuntimeValue& value);
bool runtimeValueIsStorable(const RuntimeValue& value);
RuntimeValueContractResult validateRuntimeValueContract(
    const RuntimeValue& value);

std::string runtimeFunctionHandleText(const RuntimeValue& value);
RuntimeValue runtimeFunctionHandleMetadata(const RuntimeValue& value);
std::string runtimeValueToString(const RuntimeValue& value);

} // namespace mparser
