#pragma once

#include "mparser/frontend/source.h"

#include <cstddef>
#include <cstdint>
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
  MissingArray,
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
  Single,
  Int8,
  UInt8,
  Int16,
  UInt16,
  Int32,
  UInt32,
  Int64,
  UInt64,
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
struct RuntimeCategoricalStorage;
struct RuntimeSparseStorage;
struct RuntimeTabularStorage;
struct RuntimeDynamicPropertyOwner;

struct RuntimeValue {
  RuntimeValueKind kind = RuntimeValueKind::Missing;
  double number = 0.0;
  std::string text;
  std::vector<double> elements;
  std::vector<double> imaginaryElements;
  std::vector<std::uint64_t> exactIntegerElements;
  std::vector<std::uint64_t> exactIntegerImaginaryElements;
  std::u16string characterElements;
  std::vector<RuntimeStringElement> stringElements;
  std::vector<RuntimeValue> cells;
  std::string className;
  // User-defined class values retain the callable context that owns their
  // method definitions. Builtin opaque object families leave this empty.
  std::shared_ptr<RuntimeCallableContext> objectContext;
  std::string enumerationMemberName;
  std::map<std::string, RuntimeValue> fields;
  std::vector<std::map<std::string, RuntimeValue>> structElements;
  std::vector<RuntimeValue> objectElements;
  std::vector<std::string> fieldOrder;
  std::shared_ptr<std::map<std::string, RuntimeValue>> sharedFields;
  std::shared_ptr<RuntimeDynamicPropertyOwner> dynamicPropertyOwner;
  std::shared_ptr<RuntimeFunctionHandle> functionHandle;
  // Sparse numeric payloads are shared and copied on write, just like other
  // immutable runtime values.  The payload is intentionally opaque to the
  // public C ABI and machine result protocol.
  std::shared_ptr<RuntimeSparseStorage> sparseStorage;
  // Categorical payloads keep a shared dictionary and compact codes. Code 0
  // represents MATLAB's undefined categorical element.
  std::shared_ptr<RuntimeCategoricalStorage> categoricalStorage;
  // Table and timetable payloads share immutable copy-on-write storage.
  // Their layout stays internal to the runtime and is opaque to the C ABI.
  std::shared_ptr<RuntimeTabularStorage> tabularStorage;
  bool handleObject = false;
  size_t opaqueId = 0;
  size_t rows = 0;
  size_t columns = 0;
  std::vector<size_t> dimensions;
  RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
  bool numericComplex = false;
};

using RuntimeWorkspace = std::map<std::string, RuntimeValue>;

// Descriptors must survive invocation boundaries without keeping owners alive.
struct RuntimeDynamicPropertyOwner {
  std::weak_ptr<RuntimeWorkspace> fields;
  std::string className;
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
  bool externalMethodDispatch = false;
  bool externalMemberResolution = false;
  bool externalMemberAssignment = false;
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
RuntimeValue makeRuntimeMissingArrayValue(
    std::vector<size_t> dimensions = {1, 1});
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
RuntimeWorkspace captureRuntimeWorkspace(
    const RuntimeWorkspace& workspace,
    const std::vector<std::string>& captureNames);

std::string_view runtimeValueKindName(RuntimeValueKind kind);
std::string_view runtimeValueOwnershipName(
    RuntimeValueOwnership ownership);
RuntimeValueOwnership runtimeValueOwnership(const RuntimeValue& value);
bool runtimeValueIsStorable(const RuntimeValue& value);
RuntimeValueContractResult validateRuntimeValueContract(
    const RuntimeValue& value);
std::optional<size_t> runtimeValueArrayBytes(
    const RuntimeValue& value);

std::string runtimeFunctionHandleText(const RuntimeValue& value);
RuntimeValue runtimeFunctionHandleMetadata(const RuntimeValue& value);
std::string runtimeValueToString(const RuntimeValue& value);

} // namespace mparser
