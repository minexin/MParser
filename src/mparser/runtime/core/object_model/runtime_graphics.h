#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <string>
#include <variant>

namespace mparser {

struct RuntimeValue;
struct RuntimeObjectOperationResult;

using RuntimeGraphicsId = std::uint64_t;
enum class RuntimeGraphicsKind { Figure, Axes, Line };
using RuntimeGraphicsProperty = std::variant<std::string, std::vector<double>>;

struct RuntimeGraphicsNode {
    RuntimeGraphicsKind kind;
    RuntimeGraphicsId id;
    std::optional<RuntimeGraphicsId> parent;
    std::vector<RuntimeGraphicsId> children;
    std::map<std::string, RuntimeGraphicsProperty> properties;
};

// Nodes contain IDs only. Exported handles may retain the graph, but the graph
// never retains a Runtime, module, or exported handle.
class RuntimeGraphicsGraph {
public:
    // Raw node views require this guard throughout their use.
    std::unique_lock<std::recursive_mutex> lock() const { return std::unique_lock(mutex_); }
    RuntimeGraphicsId create(RuntimeGraphicsKind kind,
        std::optional<RuntimeGraphicsId> parent = std::nullopt);
    const RuntimeGraphicsNode* find(RuntimeGraphicsId id) const noexcept;
    void reparent(RuntimeGraphicsId id, RuntimeGraphicsId parent);
    void erase(RuntimeGraphicsId id);
    void setProperty(RuntimeGraphicsId id, const std::string& name, RuntimeGraphicsProperty value);
    RuntimeGraphicsId plot(std::vector<double> y);
    RuntimeGraphicsId plot(std::vector<double> x, std::vector<double> y);
    std::string serialize() const;
    const std::map<RuntimeGraphicsId, RuntimeGraphicsNode>& nodes() const noexcept { return nodes_; }
private:
    mutable std::recursive_mutex mutex_;
    void validateParent(RuntimeGraphicsKind kind, std::optional<RuntimeGraphicsId> parent) const;
    std::map<RuntimeGraphicsId, RuntimeGraphicsNode> nodes_;
    RuntimeGraphicsId nextId_ = 1;
};

// An immutable, serializer-produced document, independent of graph lifetime.
class RuntimeGraphicsSnapshot {
public:
    explicit RuntimeGraphicsSnapshot(const RuntimeGraphicsGraph& graph);
    const std::string& json() const noexcept { return json_; }
private:
    std::string json_;
};

class RuntimeGraphicsHandle {
public:
    RuntimeGraphicsHandle(std::shared_ptr<RuntimeGraphicsGraph> graph, RuntimeGraphicsId id);
    bool valid() const;
    RuntimeGraphicsId id() const noexcept { return id_; }
    RuntimeGraphicsKind kind() const;
    // Versioned graph plus a record-local reference to this handle. Deleted
    // handles have a null reference; no process address or historical ID leaks.
    std::string serialize() const;
    bool sameIdentity(const RuntimeGraphicsHandle& other) const noexcept;
    std::optional<RuntimeGraphicsHandle> parent() const;
    std::vector<RuntimeGraphicsHandle> children() const;
    void reparent(const RuntimeGraphicsHandle& parent);
    void erase();
    RuntimeGraphicsProperty property(const std::string& name) const;
    void setProperty(const std::string& name, RuntimeGraphicsProperty value);
private:
    friend std::string serializeRuntimeGraphicsValue(const RuntimeValue& value);
    const RuntimeGraphicsNode& requireNode() const;
    std::shared_ptr<RuntimeGraphicsGraph> graph_;
    RuntimeGraphicsId id_;
};

RuntimeValue makeRuntimeGraphicsValue(RuntimeGraphicsHandle handle);
bool isRuntimeGraphicsValue(const RuntimeValue& value);
std::string serializeRuntimeGraphicsValue(const RuntimeValue& value);
RuntimeObjectOperationResult runtimeGraphicsMember(const RuntimeValue& value, const std::string& name);
RuntimeObjectOperationResult runtimeSetGraphicsMember(const RuntimeValue& target,
    const std::string& name, const RuntimeValue& value);

} // namespace mparser
