#include "mparser/runtime/core/object_model/runtime_graphics.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <set>
#include <nlohmann/json.hpp>

namespace mparser {
namespace {
std::vector<double> dataLimits(const std::vector<double>& data) {
    double lower = std::numeric_limits<double>::infinity();
    double upper = -lower;
    for (double value : data) {
        if (std::isfinite(value)) { lower = std::min(lower, value); upper = std::max(upper, value); }
    }
    if (!std::isfinite(lower)) { return {0, 1}; }
    if (lower == upper) {
        const double padding = std::max(1.0, std::abs(lower) * 0.05);
        const double paddedLower = lower - padding;
        const double paddedUpper = upper + padding;
        if (std::isfinite(paddedLower)) { lower = paddedLower; }
        if (std::isfinite(paddedUpper)) { upper = paddedUpper; }
    }
    return {lower, upper};
}

std::map<std::string, RuntimeGraphicsProperty> defaultProperties(RuntimeGraphicsKind kind) {
    if (kind == RuntimeGraphicsKind::Figure) {
        return {{"Name", std::string{}}, {"Visible", std::string{"on"}}};
    }
    if (kind == RuntimeGraphicsKind::Axes) {
        return {{"XLim", std::vector<double>{0, 1}}, {"YLim", std::vector<double>{0, 1}},
            {"XLabel", std::string{}}, {"YLabel", std::string{}}, {"Title", std::string{}}};
    }
    return {{"XData", std::vector<double>{}}, {"YData", std::vector<double>{}},
        {"Color", std::vector<double>{0, 0.447, 0.741}}, {"LineStyle", std::string{"-"}},
        {"DisplayName", std::string{}}};
}
}

const RuntimeGraphicsNode* RuntimeGraphicsGraph::find(RuntimeGraphicsId id) const noexcept {
    const auto node = nodes_.find(id);
    return node == nodes_.end() ? nullptr : &node->second;
}

void RuntimeGraphicsGraph::validateParent(RuntimeGraphicsKind kind,
    std::optional<RuntimeGraphicsId> parent) const {
    if (kind == RuntimeGraphicsKind::Figure) {
        if (parent) { throw std::invalid_argument("Figure cannot have a parent"); }
        return;
    }
    if (kind != RuntimeGraphicsKind::Axes && kind != RuntimeGraphicsKind::Line) {
        throw std::invalid_argument("unknown graphics node kind");
    }
    const auto* node = parent ? find(*parent) : nullptr;
    const auto expected = kind == RuntimeGraphicsKind::Axes
        ? RuntimeGraphicsKind::Figure : RuntimeGraphicsKind::Axes;
    if (!node || node->kind != expected) {
        throw std::invalid_argument("invalid graphics parent");
    }
}

RuntimeGraphicsId RuntimeGraphicsGraph::create(RuntimeGraphicsKind kind,
    std::optional<RuntimeGraphicsId> parent) {
    const auto guard = lock();
    validateParent(kind, parent);
    if (nextId_ == std::numeric_limits<RuntimeGraphicsId>::max()) {
        throw std::overflow_error("graphics identity space exhausted");
    }
    const auto id = nextId_;
    // Reserve before insertion so allocation failure cannot leave a one-sided edge.
    if (parent) {
        auto& children = nodes_.at(*parent).children;
        children.reserve(children.size() + 1);
    }
    nodes_.emplace(id, RuntimeGraphicsNode{kind, id, parent, {}, defaultProperties(kind)});
    if (parent) { nodes_.at(*parent).children.push_back(id); }
    ++nextId_;
    return id;
}

void RuntimeGraphicsGraph::reparent(RuntimeGraphicsId id, RuntimeGraphicsId parent) {
    const auto guard = lock();
    const auto* found = find(id);
    if (!found) { throw std::invalid_argument("deleted or unknown graphics object"); }
    validateParent(found->kind, parent);
    if (found->parent == parent) { return; }
    auto& destination = nodes_.at(parent).children;
    destination.reserve(destination.size() + 1);
    auto& node = nodes_.at(id);
    if (node.parent) { std::erase(nodes_.at(*node.parent).children, id); }
    destination.push_back(id);
    node.parent = parent;
}

RuntimeGraphicsId RuntimeGraphicsGraph::plot(std::vector<double> y) {
    std::vector<double> x(y.size());
    for (size_t index = 0; index < x.size(); ++index) {
        x[index] = static_cast<double>(index) + 1;
    }
    return plot(std::move(x), std::move(y));
}

std::string RuntimeGraphicsGraph::serialize() const {
    const auto guard = lock();
    using Json = nlohmann::ordered_json;
    std::map<RuntimeGraphicsId, size_t> ids;
    for (const auto& [id, node] : nodes_) {
        if (node.id != id) { throw std::logic_error("graphics node identity mismatch"); }
        ids.emplace(id, ids.size() + 1);
    }
    Json objects = Json::array();
    for (const auto& [id, node] : nodes_) {
        validateParent(node.kind, node.parent);
        std::set<RuntimeGraphicsId> uniqueChildren;
        Json children = Json::array();
        for (auto child : node.children) {
            const auto* target = find(child);
            if (!target || target->parent != id || !uniqueChildren.insert(child).second) {
                throw std::logic_error("invalid graphics child reference");
            }
            children.push_back(ids.at(child));
        }
        if (node.parent) {
            const auto& siblings = nodes_.at(*node.parent).children;
            if (std::count(siblings.begin(), siblings.end(), id) != 1) {
                throw std::logic_error("invalid graphics parent reference");
            }
        }
        Json properties = Json::object();
        for (const auto& [name, value] : node.properties) {
            if (const auto* text = std::get_if<std::string>(&value)) {
                properties[name] = *text;
            } else {
                Json numbers = Json::array();
                for (double number : std::get<std::vector<double>>(value)) {
                    if (std::isnan(number)) { numbers.push_back("NaN"); }
                    else if (std::isinf(number)) { numbers.push_back(number < 0 ? "-Inf" : "Inf"); }
                    else { numbers.push_back(number == 0 ? 0.0 : number); }
                }
                properties[name] = std::move(numbers);
            }
        }
        const char* kind = node.kind == RuntimeGraphicsKind::Figure ? "Figure" :
            node.kind == RuntimeGraphicsKind::Axes ? "Axes" : "Line";
        objects.push_back({{"id", ids.at(id)}, {"type", kind},
            {"parent", node.parent ? Json(ids.at(*node.parent)) : Json(nullptr)},
            {"children", std::move(children)}, {"properties", std::move(properties)}});
    }
    return Json{{"schema", "mparser.graphics"}, {"version", 1},
        {"objects", std::move(objects)}}.dump();
}

RuntimeGraphicsSnapshot::RuntimeGraphicsSnapshot(const RuntimeGraphicsGraph& graph)
    : json_(graph.serialize()) {}

RuntimeGraphicsId RuntimeGraphicsGraph::plot(std::vector<double> x, std::vector<double> y) {
    const auto guard = lock();
    if (x.size() != y.size()) { throw std::invalid_argument("plot coordinates must have equal lengths"); }
    // Construct the complete tree separately. Allocation and validation failures
    // leave the existing graph and its next identity unchanged.
    RuntimeGraphicsGraph pending;
    pending.nextId_ = nextId_;
    const auto figure = pending.create(RuntimeGraphicsKind::Figure);
    const auto axes = pending.create(RuntimeGraphicsKind::Axes, figure);
    const auto line = pending.create(RuntimeGraphicsKind::Line, axes);
    pending.setProperty(axes, "XLim", dataLimits(x));
    pending.setProperty(axes, "YLim", dataLimits(y));
    pending.setProperty(line, "XData", std::move(x));
    pending.setProperty(line, "YData", std::move(y));
    const auto newNext = pending.nextId_;
    nodes_.merge(pending.nodes_);
    nextId_ = newNext;
    return line;
}

void RuntimeGraphicsGraph::setProperty(RuntimeGraphicsId id, const std::string& name,
    RuntimeGraphicsProperty value) {
    const auto guard = lock();
    auto found = nodes_.find(id);
    if (found == nodes_.end()) { throw std::invalid_argument("deleted or unknown graphics object"); }
    auto property = found->second.properties.find(name);
    if (property == found->second.properties.end() || property->second.index() != value.index()) {
        throw std::invalid_argument("unknown graphics property or incorrect value type");
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        if (name == "Visible" && *text != "on" && *text != "off") {
            throw std::invalid_argument("Visible must be on or off");
        }
        if (name == "LineStyle" && *text != "-" && *text != "--" && *text != ":" &&
            *text != "-." && *text != "none") {
            throw std::invalid_argument("invalid LineStyle");
        }
    } else {
        const auto& data = std::get<std::vector<double>>(value);
        if (name == "Color" && (data.size() != 3 || std::any_of(data.begin(), data.end(),
                [](double channel) { return !std::isfinite(channel) || channel < 0 || channel > 1; }))) {
            throw std::invalid_argument("Color requires three finite channels in [0,1]");
        }
        if ((name == "XLim" || name == "YLim") && (data.size() != 2 ||
            !std::isfinite(data[0]) || !std::isfinite(data[1]) || data[0] >= data[1])) {
            throw std::invalid_argument("axes limits require two increasing finite values");
        }
    }
    property->second = std::move(value);
}

void RuntimeGraphicsGraph::erase(RuntimeGraphicsId id) {
    const auto guard = lock();
    const auto* root = find(id);
    if (!root) { return; }
    std::vector<RuntimeGraphicsId> pending{id};
    for (size_t index = 0; index < pending.size(); ++index) {
        const auto& children = nodes_.at(pending[index]).children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    if (root->parent) { std::erase(nodes_.at(*root->parent).children, id); }
    for (auto node : pending) { nodes_.erase(node); }
}

RuntimeGraphicsHandle::RuntimeGraphicsHandle(std::shared_ptr<RuntimeGraphicsGraph> graph,
    RuntimeGraphicsId id) : graph_(std::move(graph)), id_(id) {
    if (!graph_) { throw std::invalid_argument("missing graphics graph"); }
    const auto guard = graph_->lock();
    (void)requireNode();
}

bool RuntimeGraphicsHandle::valid() const {
    if (!graph_) { return false; }
    const auto guard = graph_->lock();
    return graph_->find(id_) != nullptr;
}

RuntimeGraphicsKind RuntimeGraphicsHandle::kind() const {
    const auto guard = graph_->lock();
    return requireNode().kind;
}

std::string RuntimeGraphicsHandle::serialize() const {
    const auto guard = graph_->lock();
    using Json = nlohmann::ordered_json;
    Json reference = nullptr;
    if (valid()) {
        size_t index = 1;
        for (const auto& [id, node] : graph_->nodes()) {
            (void)node;
            if (id == id_) { reference = index; break; }
            ++index;
        }
    }
    return Json{{"valid", valid()}, {"reference", std::move(reference)},
        {"graph", Json::parse(graph_->serialize())}}.dump(-1, ' ', true);
}

bool isRuntimeGraphicsValue(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::Object || !value.handleObject || value.objectContext ||
        (value.className != "matlab.ui.Figure" && value.className != "matlab.graphics.axis.Axes" &&
         value.className != "matlab.graphics.chart.primitive.Line")) { return false; }
    if (value.graphicsHandle) { return true; }
    if (value.objectElements.empty()) { return runtimeShapeElementCount(value) == 0; }
    return std::any_of(value.objectElements.begin(), value.objectElements.end(),
        [](const RuntimeValue& element) { return element.graphicsHandle != nullptr; });
}

std::string serializeRuntimeGraphicsValue(const RuntimeValue& value) {
    if (!isRuntimeGraphicsValue(value)) { throw std::invalid_argument("expected graphics value"); }
    const auto dimensions = runtimeDimensions(value);
    const auto product = checkedRuntimeDimensionProduct(dimensions);
    if (!product || runtimeObjectElementCount(value) != *product ||
        (value.graphicsHandle && (*product != 1 || !value.objectElements.empty()))) {
        throw std::invalid_argument("graphics payload does not match its shape");
    }
    if (value.graphicsHandle) { return value.graphicsHandle->serialize(); }
    using Json = nlohmann::ordered_json;
    std::vector<const RuntimeGraphicsHandle*> handles;
    std::vector<std::shared_ptr<RuntimeGraphicsGraph>> graphs;
    const auto count = *product;
    for (size_t index = 0; index < count; ++index) {
        const auto* element = runtimeObjectLogicalElement(value, index);
        if (!element || !element->graphicsHandle) {
            throw std::invalid_argument("graphics array contains a non-graphics element");
        }
        const auto* handle = element->graphicsHandle.get();
        handles.push_back(handle);
        if (std::find(graphs.begin(), graphs.end(), handle->graph_) == graphs.end()) {
            graphs.push_back(handle->graph_);
        }
    }
    // Lock by address, but publish graphs by first logical occurrence.
    auto ordered = graphs;
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return std::less<const RuntimeGraphicsGraph*>{}(left.get(), right.get());
    });
    std::vector<std::unique_lock<std::recursive_mutex>> guards;
    for (const auto& graph : ordered) { guards.push_back(graph->lock()); }
    Json records = Json::array();
    std::vector<std::map<RuntimeGraphicsId, size_t>> references;
    for (const auto& graph : graphs) {
        records.push_back(Json::parse(graph->serialize()));
        auto& ids = references.emplace_back();
        for (const auto& [id, node] : graph->nodes()) {
            (void)node;
            ids.emplace(id, ids.size() + 1);
        }
    }
    Json elements = Json::array();
    for (const auto* handle : handles) {
        const auto graphIndex = static_cast<size_t>(
            std::find(graphs.begin(), graphs.end(), handle->graph_) - graphs.begin());
        const auto& ids = references.at(graphIndex);
        const auto found = ids.find(handle->id_);
        const bool valid = found != ids.end();
        elements.push_back({{"graph", graphIndex + 1}, {"valid", valid},
            {"reference", valid ? Json(found->second) : Json(nullptr)}});
    }
    return Json{{"dimensions", dimensions}, {"graphs", std::move(records)},
        {"elements", std::move(elements)}}.dump(-1, ' ', true);
}

RuntimeObjectOperationResult runtimeGraphicsMember(const RuntimeValue& value, const std::string& name) {
    try {
        if (!value.graphicsHandle) { throw std::invalid_argument("expected a graphics handle"); }
        const auto& handle = *value.graphicsHandle;
        if (name == "Parent") {
            auto parent = handle.parent();
            return {true, parent ? makeRuntimeGraphicsValue(*parent) : makeRuntimeMatrixValue(0,0,{}), {}};
        }
        if (name == "Children") {
            std::vector<RuntimeValue> children;
            for (auto child : handle.children()) { children.push_back(makeRuntimeGraphicsValue(std::move(child))); }
            if (children.empty()) { return {true, makeRuntimeMatrixValue(0,0,{}), {}}; }
            const auto count = children.size();
            const auto className = children.front().className;
            return runtimeMakeObjectArrayFromLogicalOrder(std::move(children), {count,1}, className, true);
        }
        auto property = handle.property(name);
        if (const auto* text = std::get_if<std::string>(&property)) {
            return {true, makeRuntimeCharacterVectorUtf8(*text), {}};
        }
        return {true, makeRuntimeVectorValue(std::get<std::vector<double>>(std::move(property))), {}};
    } catch (const std::invalid_argument& error) {
        return {false, {}, error.what()};
    }
}

RuntimeObjectOperationResult runtimeSetGraphicsMember(const RuntimeValue& target,
    const std::string& name, const RuntimeValue& value) {
    try {
        if (!target.graphicsHandle) { throw std::invalid_argument("expected a graphics handle"); }
        auto& handle = *target.graphicsHandle;
        if (name == "Parent") {
            if (!value.graphicsHandle) { throw std::invalid_argument("Parent requires a graphics handle"); }
            handle.reparent(*value.graphicsHandle);
        } else if (name == "Children") {
            throw std::invalid_argument("Children is read-only; assign Parent to reparent an object");
        } else if (const auto text = runtimeTextScalarUtf8(value)) {
            handle.setProperty(name, *text);
        } else {
            const auto dimensions = runtimeDimensions(value);
            const auto count = runtimeShapeElementCount(value);
            if (!isRuntimeNumericValue(value) || value.numericComplex || value.sparseStorage ||
                dimensions.size() != 2 || (count && dimensions[0] != 1 && dimensions[1] != 1)) {
                throw std::invalid_argument("graphics property requires text or a real dense numeric vector");
            }
            std::vector<double> numbers;
            numbers.reserve(count);
            for (size_t index = 0; index < count; ++index) {
                auto number = runtimeNumericElement(value, index);
                if (!number) { throw std::invalid_argument("invalid numeric property value"); }
                numbers.push_back(*number);
            }
            handle.setProperty(name, std::move(numbers));
        }
        return {true, target, {}};
    } catch (const std::invalid_argument& error) {
        return {false, {}, error.what()};
    }
}

RuntimeValue makeRuntimeGraphicsValue(RuntimeGraphicsHandle handle) {
    const auto kind = handle.kind();
    const char* className = kind == RuntimeGraphicsKind::Figure
        ? "matlab.ui.Figure" : kind == RuntimeGraphicsKind::Axes
        ? "matlab.graphics.axis.Axes" : "matlab.graphics.chart.primitive.Line";
    auto value = makeRuntimeObjectScalar(className, {}, true);
    value.graphicsHandle = std::make_shared<RuntimeGraphicsHandle>(std::move(handle));
    return value;
}

const RuntimeGraphicsNode& RuntimeGraphicsHandle::requireNode() const {
    const auto* node = graph_ ? graph_->find(id_) : nullptr;
    if (!node) { throw std::invalid_argument("deleted or unknown graphics handle"); }
    return *node;
}

bool RuntimeGraphicsHandle::sameIdentity(const RuntimeGraphicsHandle& other) const noexcept {
    return graph_ == other.graph_ && id_ == other.id_;
}

std::optional<RuntimeGraphicsHandle> RuntimeGraphicsHandle::parent() const {
    const auto guard = graph_->lock();
    const auto& node = requireNode();
    if (!node.parent) { return std::nullopt; }
    return RuntimeGraphicsHandle(graph_, *node.parent);
}

std::vector<RuntimeGraphicsHandle> RuntimeGraphicsHandle::children() const {
    const auto guard = graph_->lock();
    const auto& node = requireNode();
    std::vector<RuntimeGraphicsHandle> result;
    result.reserve(node.children.size());
    for (auto child : node.children) { result.emplace_back(graph_, child); }
    return result;
}

void RuntimeGraphicsHandle::reparent(const RuntimeGraphicsHandle& parent) {
    if (graph_ != parent.graph_) { throw std::invalid_argument("foreign graphics parent"); }
    const auto guard = graph_->lock();
    (void)requireNode();
    (void)parent.requireNode();
    graph_->reparent(id_, parent.id_);
}

void RuntimeGraphicsHandle::erase() { if (graph_) { graph_->erase(id_); } }

RuntimeGraphicsProperty RuntimeGraphicsHandle::property(const std::string& name) const {
    const auto guard = graph_->lock();
    const auto& properties = requireNode().properties;
    const auto found = properties.find(name);
    if (found == properties.end()) { throw std::invalid_argument("unknown graphics property"); }
    return found->second;
}

void RuntimeGraphicsHandle::setProperty(const std::string& name, RuntimeGraphicsProperty value) {
    const auto guard = graph_->lock();
    (void)requireNode();
    graph_->setProperty(id_, name, std::move(value));
}

} // namespace mparser
