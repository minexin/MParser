#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <string>
#include <variant>
#include <utility>

namespace mparser {

struct RuntimeValue;
struct RuntimeObjectOperationResult;

using RuntimeGraphicsId = std::uint64_t;
enum class RuntimeGraphicsKind { Figure, Axes, Line, Text, Legend };
using RuntimeGraphicsProperty = std::variant<std::string, std::vector<double>, std::vector<std::string>>;

struct RuntimeGraphicsSeries {
    std::vector<double> x;
    std::vector<double> y;
};

struct RuntimeGraphicsLegendOptions {
    std::optional<std::vector<RuntimeGraphicsId>> lines;
    std::vector<std::string> labels;
    std::vector<std::pair<std::string, RuntimeGraphicsProperty>> properties;
};

struct RuntimeGraphicsLegendResult {
    RuntimeGraphicsId id;
    bool ignoredLabels = false;
};

struct RuntimeGraphicsLayoutEnvironment {
    double widthPixels = 0;
    double heightPixels = 0;
    double pixelsPerInch = 0;
    double characterWidthPixels = 0;
    double characterHeightPixels = 0;
};

struct RuntimeGraphicsNode {
    RuntimeGraphicsKind kind;
    RuntimeGraphicsId id;
    std::optional<RuntimeGraphicsId> parent;
    std::vector<RuntimeGraphicsId> children;
    std::map<std::string, RuntimeGraphicsProperty> properties;
    std::optional<RuntimeGraphicsId> currentAxes;
    std::map<std::string, RuntimeGraphicsId> labels;
    std::optional<RuntimeGraphicsId> associatedAxes;
    std::vector<RuntimeGraphicsId> legendLines;
    std::optional<RuntimeGraphicsLayoutEnvironment> layoutEnvironment;
    std::uint64_t layoutRevision = 0;
    bool legendExplicitMembers = false;
    std::optional<RuntimeGraphicsId> generatedLegendName = std::nullopt;
};

// Nodes contain IDs only. Exported handles may retain the graph, but the graph
// never retains a Runtime, module, or exported handle.
class RuntimeGraphicsGraph {
public:
    RuntimeGraphicsGraph();
    std::uint64_t identity() const noexcept { return identity_; }
    // Raw node views require this guard throughout their use.
    std::unique_lock<std::recursive_mutex> lock() const { return std::unique_lock(mutex_); }
    RuntimeGraphicsId create(RuntimeGraphicsKind kind,
        std::optional<RuntimeGraphicsId> parent = std::nullopt);
    const RuntimeGraphicsNode* find(RuntimeGraphicsId id) const noexcept;
    void reparent(RuntimeGraphicsId id, RuntimeGraphicsId parent);
    void erase(RuntimeGraphicsId id);
    void setProperty(RuntimeGraphicsId id, const std::string& name, RuntimeGraphicsProperty value);
    void setLayoutEnvironment(RuntimeGraphicsId figure, RuntimeGraphicsLayoutEnvironment environment);
    std::optional<RuntimeGraphicsLayoutEnvironment> layoutEnvironment(RuntimeGraphicsId figure) const;
    RuntimeGraphicsId currentFigure();
    RuntimeGraphicsId currentAxes();
    void selectFigure(RuntimeGraphicsId id);
    void selectAxes(RuntimeGraphicsId id);
    RuntimeGraphicsId setLabel(RuntimeGraphicsId axes, const std::string& role, std::string text);
    RuntimeGraphicsId legend(RuntimeGraphicsId axes);
    RuntimeGraphicsLegendResult configureLegend(std::optional<RuntimeGraphicsId> axes,
        const RuntimeGraphicsLegendOptions& options);
    RuntimeGraphicsId plot(std::vector<double> y);
    RuntimeGraphicsId plot(std::vector<double> x, std::vector<double> y);
    std::vector<RuntimeGraphicsId> plotLines(std::vector<RuntimeGraphicsSeries> series);
    // Opt-in v3 identities are scoped to this loaded kernel instance.
    std::string serialize(bool persistentIdentity = false, bool ascii = false, bool includeLayout = false) const;
    const std::map<RuntimeGraphicsId, RuntimeGraphicsNode>& nodes() const noexcept { return nodes_; }
private:
    mutable std::recursive_mutex mutex_;
    const std::uint64_t identity_;
    void validateParent(RuntimeGraphicsKind kind, std::optional<RuntimeGraphicsId> parent) const;
    void refreshLegendName(RuntimeGraphicsId line);
    std::map<RuntimeGraphicsId, RuntimeGraphicsNode> nodes_;
    RuntimeGraphicsId nextId_ = 1;
    std::optional<RuntimeGraphicsId> currentFigure_;
};

// An immutable, serializer-produced document, independent of graph lifetime.
class RuntimeGraphicsSnapshot {
public:
    explicit RuntimeGraphicsSnapshot(const RuntimeGraphicsGraph& graph);
    const std::string& json(bool persistentIdentity = false, bool includeLayout = false) const noexcept {
        return includeLayout ? layoutJson_ : persistentIdentity ? identityJson_ : json_;
    }
private:
    std::string json_;
    std::string identityJson_;
    std::string layoutJson_;
};

class RuntimeGraphicsHandle {
public:
    RuntimeGraphicsHandle(std::shared_ptr<RuntimeGraphicsGraph> graph, RuntimeGraphicsId id);
    bool valid() const;
    bool belongsTo(const RuntimeGraphicsGraph& graph) const noexcept { return graph_.get() == &graph; }
    RuntimeGraphicsId id() const noexcept { return id_; }
    RuntimeGraphicsKind kind() const;
    // Versioned graph plus a record-local reference to this handle. Deleted
    // handles have a null reference; no process address or historical ID leaks.
    std::string serialize(bool persistentIdentity = false, bool includeLayout = false) const;
    bool sameIdentity(const RuntimeGraphicsHandle& other) const noexcept;
    std::optional<RuntimeGraphicsHandle> parent() const;
    // Public Children omits bound labels; graph ownership retains them.
    std::vector<RuntimeGraphicsHandle> children() const;
    void reparent(const RuntimeGraphicsHandle& parent);
    void erase();
    RuntimeGraphicsProperty property(const std::string& name) const;
    RuntimeGraphicsHandle label(const std::string& role) const;
    bool hasLabel(const std::string& role, const RuntimeGraphicsHandle& label) const;
    void setProperty(const std::string& name, RuntimeGraphicsProperty value);
private:
    friend std::string serializeRuntimeGraphicsValue(const RuntimeValue& value, bool persistentIdentity, bool includeLayout);
    const RuntimeGraphicsNode& requireNode() const;
    std::shared_ptr<RuntimeGraphicsGraph> graph_;
    RuntimeGraphicsId id_;
};

RuntimeValue makeRuntimeGraphicsValue(RuntimeGraphicsHandle handle);
bool isRuntimeGraphicsValue(const RuntimeValue& value);
std::vector<std::string> runtimeGraphicsLegendLabels(const RuntimeValue& value);
std::string serializeRuntimeGraphicsValue(const RuntimeValue& value, bool persistentIdentity = false, bool includeLayout = false);
RuntimeObjectOperationResult runtimeGraphicsMember(const RuntimeValue& value, const std::string& name);
RuntimeObjectOperationResult runtimeGraphicsLifecycle(const RuntimeValue& value, bool erase);
RuntimeObjectOperationResult runtimeSetGraphicsMember(const RuntimeValue& target,
    const std::string& name, const RuntimeValue& value);

} // namespace mparser
