#pragma once

#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/value/runtime_sparse.h"
#include "mparser/runtime/core/value/runtime_tabular.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace mparser {

using RuntimeSnapshotId = size_t;
using RuntimeSnapshotField = std::pair<std::string, RuntimeSnapshotId>;

enum class RuntimeSnapshotFailure { InvalidValue, DepthLimit, NodeLimit, ByteLimit };

class RuntimeSnapshotError : public std::runtime_error {
public:
    RuntimeSnapshotError(RuntimeSnapshotFailure reason, std::string message)
        : std::runtime_error(std::move(message)), reason_(reason) {}
    RuntimeSnapshotFailure reason() const noexcept { return reason_; }
private:
    RuntimeSnapshotFailure reason_;
};

struct RuntimeSnapshotLimits {
    size_t maximumDepth = 128;
    size_t maximumNodes = 100000;
    size_t maximumBytes = 64 * 1024 * 1024;
};

struct RuntimeSnapshotCallable {
    RuntimeFunctionHandleKind kind = RuntimeFunctionHandleKind::Function;
    RuntimeFunctionHandleBackend backend = RuntimeFunctionHandleBackend::Independent;
    std::string display, targetName, className, methodName, declaringClass;
    bool moduleBound = false;
    std::vector<std::string> parameters;
    std::optional<RuntimeSnapshotId> receiver;
    std::vector<RuntimeSnapshotField> captures;
};

struct RuntimeSnapshotTabular {
    RuntimeTabularKind kind = RuntimeTabularKind::Table;
    size_t rowCount = 0;
    RuntimeTabularRowAxisKind rowAxisKind = RuntimeTabularRowAxisKind::None;
    std::vector<std::string> rowNames, dimensionNames;
    std::string description;
    std::vector<RuntimeSnapshotField> variables;
    RuntimeSnapshotId rowTimes = 0, userData = 0;
};

struct RuntimeSnapshotWeakOwner {
    std::string className;
    bool alive = false;
};

// No RuntimeValue, shared field pointer, callback, context or writable handle is
// stored here. IDs refer only to nodes in this one immutable snapshot.
struct RuntimeSnapshotNode {
    RuntimeValueKind kind = RuntimeValueKind::Missing;
    std::vector<size_t> dimensions;
    std::string className, text, enumerationMemberName;
    bool handleObject = false, moduleBound = false;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
    bool numericComplex = false;
    // Dense numeric, UTF-16, strings, cells, structs and object arrays use
    // logical column-major order. Sparse values retain their CSC payload.
    std::vector<RuntimeNumericElementValue> numeric;
    std::vector<double> temporal;
    std::u16string characters;
    std::vector<RuntimeStringElement> strings;
    std::vector<RuntimeSnapshotId> elements;
    std::vector<std::string> fieldOrder;
    std::vector<std::vector<RuntimeSnapshotField>> structElements;
    std::vector<RuntimeSnapshotField> storedFields;
    std::optional<RuntimeSparseStorage> sparse;
    std::optional<RuntimeCategoricalStorage> categorical;
    std::optional<RuntimeSnapshotTabular> tabular;
    std::optional<RuntimeSnapshotCallable> callable;
    std::optional<RuntimeSnapshotWeakOwner> weakOwner;
    std::string graphicsJson;
};

class RuntimeValueSnapshot {
public:
    RuntimeValueSnapshot() = default;
    static RuntimeValueSnapshot capture(const RuntimeValue& value, RuntimeSnapshotLimits limits = {});
    bool available() const noexcept { return static_cast<bool>(nodes_); }
    size_t size() const noexcept { return nodes_ ? nodes_->size() : 0; }
    const RuntimeSnapshotNode& node(RuntimeSnapshotId id = 0) const;
private:
    explicit RuntimeValueSnapshot(std::vector<RuntimeSnapshotNode> nodes)
        : nodes_(std::make_shared<const std::vector<RuntimeSnapshotNode>>(std::move(nodes))) {}
    std::shared_ptr<const std::vector<RuntimeSnapshotNode>> nodes_;
};

} // namespace mparser
