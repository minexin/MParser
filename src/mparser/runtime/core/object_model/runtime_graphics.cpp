#include "mparser/runtime/core/object_model/runtime_graphics.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <set>
#include <nlohmann/json.hpp>

namespace mparser {
namespace {
std::uint64_t allocateGraphIdentity() {
    static std::atomic<std::uint64_t> next{1};
    auto candidate = next.load(std::memory_order_relaxed);
    for (;;) {
        if (candidate == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("graphics graph identity exhausted");
        }
        if (next.compare_exchange_weak(candidate, candidate + 1, std::memory_order_relaxed)) {
            return candidate;
        }
    }
}
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
            {"XLabel", std::string{}}, {"YLabel", std::string{}}, {"Title", std::string{}},
            {"NextPlot", std::string{"replace"}}, {"Clipping", std::string{"on"}},
            {"XGrid", std::string{"off"}}, {"YGrid", std::string{"off"}}};
    }
    if (kind == RuntimeGraphicsKind::Text) {
        return {{"String", std::string{}}, {"Visible", std::string{"on"}},
            {"Color", std::vector<double>{0, 0, 0}}};
    }
    if (kind == RuntimeGraphicsKind::Legend) {
        return {{"Visible", std::string{"on"}}, {"Location", std::string{"northeast"}},
            {"Orientation", std::string{"vertical"}}};
    }
    return {{"XData", std::vector<double>{}}, {"YData", std::vector<double>{}},
        {"Color", std::vector<double>{0, 0.447, 0.741}}, {"LineStyle", std::string{"-"}},
        {"DisplayName", std::string{}}, {"Clipping", std::string{"on"}}};
}
}

const RuntimeGraphicsNode* RuntimeGraphicsGraph::find(RuntimeGraphicsId id) const noexcept {
    const auto node = nodes_.find(id);
    return node == nodes_.end() ? nullptr : &node->second;
}

RuntimeGraphicsGraph::RuntimeGraphicsGraph() : identity_(allocateGraphIdentity()) {}

void RuntimeGraphicsGraph::validateParent(RuntimeGraphicsKind kind,
    std::optional<RuntimeGraphicsId> parent) const {
    if (kind == RuntimeGraphicsKind::Figure) {
        if (parent) { throw std::invalid_argument("Figure cannot have a parent"); }
        return;
    }
    if (kind != RuntimeGraphicsKind::Axes && kind != RuntimeGraphicsKind::Line &&
        kind != RuntimeGraphicsKind::Text && kind != RuntimeGraphicsKind::Legend) {
        throw std::invalid_argument("unknown graphics node kind");
    }
    const auto* node = parent ? find(*parent) : nullptr;
    const auto expected = kind == RuntimeGraphicsKind::Axes || kind == RuntimeGraphicsKind::Legend
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
    nodes_.emplace(id, RuntimeGraphicsNode{kind, id, parent, {}, defaultProperties(kind),
        std::nullopt, {}, std::nullopt});
    if (parent) { nodes_.at(*parent).children.push_back(id); }
    ++nextId_;
    if (kind == RuntimeGraphicsKind::Figure) { currentFigure_ = id; }
    if (kind == RuntimeGraphicsKind::Axes) {
        nodes_.at(*parent).currentAxes = id;
        currentFigure_ = parent;
    }
    return id;
}

RuntimeGraphicsId RuntimeGraphicsGraph::currentFigure() {
    const auto guard = lock();
    if (currentFigure_ && find(*currentFigure_)) { return *currentFigure_; }
    for (auto iterator = nodes_.rbegin(); iterator != nodes_.rend(); ++iterator) {
        if (iterator->second.kind == RuntimeGraphicsKind::Figure) {
            currentFigure_ = iterator->first;
            return *currentFigure_;
        }
    }
    return create(RuntimeGraphicsKind::Figure);
}

RuntimeGraphicsId RuntimeGraphicsGraph::currentAxes() {
    const auto guard = lock();
    const auto figure = currentFigure();
    const auto& node = nodes_.at(figure);
    if (node.currentAxes && find(*node.currentAxes)) { return *node.currentAxes; }
    for (auto iterator = node.children.rbegin(); iterator != node.children.rend(); ++iterator) {
        if (nodes_.at(*iterator).kind == RuntimeGraphicsKind::Axes) {
            nodes_.at(figure).currentAxes = *iterator;
            return *iterator;
        }
    }
    return create(RuntimeGraphicsKind::Axes, figure);
}

void RuntimeGraphicsGraph::selectFigure(RuntimeGraphicsId id) {
    const auto guard = lock();
    const auto* node = find(id);
    if (!node || node->kind != RuntimeGraphicsKind::Figure) {
        throw std::invalid_argument("expected a live Figure");
    }
    currentFigure_ = id;
}

void RuntimeGraphicsGraph::selectAxes(RuntimeGraphicsId id) {
    const auto guard = lock();
    const auto* node = find(id);
    if (!node || node->kind != RuntimeGraphicsKind::Axes) {
        throw std::invalid_argument("expected a live Axes");
    }
    nodes_.at(*node->parent).currentAxes = id;
    currentFigure_ = node->parent;
}

RuntimeGraphicsId RuntimeGraphicsGraph::setLabel(RuntimeGraphicsId axes, const std::string& role, std::string text) {
    const auto guard = lock();
    const auto* node = find(axes);
    if (!node || node->kind != RuntimeGraphicsKind::Axes ||
        (role != "Title" && role != "XLabel" && role != "YLabel")) {
        throw std::invalid_argument("expected Axes and Title/XLabel/YLabel role");
    }
    RuntimeGraphicsGraph pending;
    pending.nodes_ = nodes_;
    pending.nextId_ = nextId_;
    pending.currentFigure_ = currentFigure_;
    auto& labels = pending.nodes_.at(axes).labels;
    const auto found = labels.find(role);
    const auto id = found == labels.end() ? pending.create(RuntimeGraphicsKind::Text, axes) : found->second;
    labels[role] = id;
    pending.setProperty(id, "String", std::move(text));
    nodes_.swap(pending.nodes_);
    nextId_ = pending.nextId_;
    return id;
}

RuntimeGraphicsId RuntimeGraphicsGraph::legend(RuntimeGraphicsId axes) {
    const auto guard = lock();
    const auto* target = find(axes);
    if (!target || target->kind != RuntimeGraphicsKind::Axes) {
        throw std::invalid_argument("legend requires a live Axes");
    }
    for (const auto& [id, node] : nodes_) {
        if (node.kind == RuntimeGraphicsKind::Legend && node.associatedAxes == axes) { return id; }
    }
    const auto id = create(RuntimeGraphicsKind::Legend, target->parent);
    nodes_.at(id).associatedAxes = axes;
    return id;
}

void RuntimeGraphicsGraph::reparent(RuntimeGraphicsId id, RuntimeGraphicsId parent) {
    const auto guard = lock();
    const auto* found = find(id);
    if (!found) { throw std::invalid_argument("deleted or unknown graphics object"); }
    validateParent(found->kind, parent);
    if (found->parent == parent) { return; }
    if (found->associatedAxes && nodes_.at(*found->associatedAxes).parent != parent) {
        throw std::invalid_argument("Legend must share its associated Axes parent");
    }
    std::vector<RuntimeGraphicsId> legends;
    if (found->kind == RuntimeGraphicsKind::Axes) {
        for (const auto& [candidate, node] : nodes_) {
            if (node.associatedAxes == id) { legends.push_back(candidate); }
        }
    }
    auto& destination = nodes_.at(parent).children;
    destination.reserve(destination.size() + 1 + legends.size());
    auto& node = nodes_.at(id);
    if (node.parent) {
        auto& previous = nodes_.at(*node.parent);
        std::erase(previous.children, id);
        if (previous.currentAxes == id) { previous.currentAxes.reset(); }
        std::erase_if(previous.labels, [id](const auto& entry) { return entry.second == id; });
    }
    destination.push_back(id);
    node.parent = parent;
    if (node.kind == RuntimeGraphicsKind::Axes) { nodes_.at(parent).currentAxes = id; }
    for (const auto legend : legends) { reparent(legend, parent); }
}

RuntimeGraphicsId RuntimeGraphicsGraph::plot(std::vector<double> y) {
    std::vector<double> x(y.size());
    for (size_t index = 0; index < x.size(); ++index) {
        x[index] = static_cast<double>(index) + 1;
    }
    return plot(std::move(x), std::move(y));
}

std::string RuntimeGraphicsGraph::serialize(bool persistentIdentity) const {
    const auto guard = lock();
    using Json = nlohmann::ordered_json;
    std::map<RuntimeGraphicsId, size_t> ids;
    for (const auto& [id, node] : nodes_) {
        if (node.id != id) { throw std::logic_error("graphics node identity mismatch"); }
        ids.emplace(id, ids.size() + 1);
    }
    Json objects = Json::array();
    bool requiresVersion2 = false;
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
            if (node.labels.contains(name)) { continue; }
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
            node.kind == RuntimeGraphicsKind::Axes ? "Axes" :
            node.kind == RuntimeGraphicsKind::Text ? "Text" :
            node.kind == RuntimeGraphicsKind::Legend ? "Legend" : "Line";
        requiresVersion2 |= node.kind == RuntimeGraphicsKind::Text || node.kind == RuntimeGraphicsKind::Legend;
        Json record{{"id", ids.at(id)}, {"type", kind},
            {"parent", node.parent ? Json(ids.at(*node.parent)) : Json(nullptr)},
            {"children", std::move(children)}, {"properties", std::move(properties)}};
        if (persistentIdentity) { record["objectIdentity"] = std::to_string(id); }
        if (!node.labels.empty()) {
            Json labels = Json::object();
            for (const auto& [role, target] : node.labels) {
                const auto* label = find(target);
                if (!label || label->kind != RuntimeGraphicsKind::Text || label->parent != id) {
                    throw std::logic_error("invalid graphics label reference");
                }
                labels[role] = ids.at(target);
            }
            record["labels"] = std::move(labels);
            requiresVersion2 = true;
        }
        if (node.associatedAxes) {
            const auto* axes = find(*node.associatedAxes);
            if (node.kind != RuntimeGraphicsKind::Legend || !axes ||
                axes->kind != RuntimeGraphicsKind::Axes || axes->parent != node.parent) {
                throw std::logic_error("invalid Legend Axes reference");
            }
            record["axes"] = ids.at(*node.associatedAxes);
            Json entries = Json::array();
            for (const auto child : axes->children) {
                const auto& series = nodes_.at(child);
                if (series.kind != RuntimeGraphicsKind::Line) { continue; }
                const auto& name = std::get<std::string>(series.properties.at("DisplayName"));
                entries.push_back({{"line", ids.at(child)}, {"label", name.empty()
                    ? "data" + std::to_string(entries.size() + 1) : name}});
            }
            record["entries"] = std::move(entries);
        }
        objects.push_back(std::move(record));
    }
    Json snapshot{{"schema", "mparser.graphics"},
        {"version", persistentIdentity ? 3 : requiresVersion2 ? 2 : 1},
        {"objects", std::move(objects)}};
    if (persistentIdentity) { snapshot["graphIdentity"] = std::to_string(identity_); }
    return snapshot.dump();
}

RuntimeGraphicsSnapshot::RuntimeGraphicsSnapshot(const RuntimeGraphicsGraph& graph)
{
    const auto guard = graph.lock();
    json_ = graph.serialize();
    identityJson_ = graph.serialize(true);
}

RuntimeGraphicsId RuntimeGraphicsGraph::plot(std::vector<double> x, std::vector<double> y) {
    std::vector<RuntimeGraphicsSeries> series;
    series.push_back({std::move(x), std::move(y)});
    return plotLines(std::move(series)).front();
}

std::vector<RuntimeGraphicsId> RuntimeGraphicsGraph::plotLines(std::vector<RuntimeGraphicsSeries> series) {
    const auto guard = lock();
    for (const auto& line : series) {
        if (line.x.size() != line.y.size()) {
            throw std::invalid_argument("plot coordinates must have equal lengths");
        }
    }
    // Stage the update so allocation and validation failures preserve live
    // handles, current selection, and the next identity together.
    RuntimeGraphicsGraph pending;
    pending.nodes_ = nodes_;
    pending.nextId_ = nextId_;
    pending.currentFigure_ = currentFigure_;
    const auto axes = pending.currentAxes();
    auto& axesNode = pending.nodes_.at(axes);
    const auto nextPlot = std::get<std::string>(axesNode.properties.at("NextPlot"));
    if (nextPlot != "add") {
        const auto children = axesNode.children;
        for (const auto child : children) {
            const bool boundLabel = std::any_of(axesNode.labels.begin(), axesNode.labels.end(),
                [child](const auto& entry) { return entry.second == child; });
            if (!boundLabel) { pending.erase(child); }
            else if (nextPlot == "replace") {
                pending.nodes_.at(child).properties = defaultProperties(RuntimeGraphicsKind::Text);
            }
        }
        if (nextPlot == "replace") { axesNode.properties = defaultProperties(RuntimeGraphicsKind::Axes); }
    }
    std::vector<RuntimeGraphicsId> lines;
    lines.reserve(series.size());
    for (auto& data : series) {
        const auto line = pending.create(RuntimeGraphicsKind::Line, axes);
        pending.setProperty(line, "XData", std::move(data.x));
        pending.setProperty(line, "YData", std::move(data.y));
        lines.push_back(line);
    }
    std::vector<double> allX, allY;
    for (const auto child : axesNode.children) {
        const auto& node = pending.nodes_.at(child);
        if (node.kind != RuntimeGraphicsKind::Line) { continue; }
        const auto& childX = std::get<std::vector<double>>(node.properties.at("XData"));
        const auto& childY = std::get<std::vector<double>>(node.properties.at("YData"));
        allX.insert(allX.end(), childX.begin(), childX.end());
        allY.insert(allY.end(), childY.begin(), childY.end());
    }
    pending.setProperty(axes, "XLim", dataLimits(allX));
    pending.setProperty(axes, "YLim", dataLimits(allY));
    nodes_.swap(pending.nodes_);
    nextId_ = pending.nextId_;
    currentFigure_ = pending.currentFigure_;
    return lines;
}

void RuntimeGraphicsGraph::setProperty(RuntimeGraphicsId id, const std::string& name,
    RuntimeGraphicsProperty value) {
    const auto guard = lock();
    auto found = nodes_.find(id);
    if (found == nodes_.end()) { throw std::invalid_argument("deleted or unknown graphics object"); }
    if (found->second.kind == RuntimeGraphicsKind::Axes &&
        (name == "Title" || name == "XLabel" || name == "YLabel")) {
        throw std::invalid_argument("Axes labels are Text objects; modify String or use a label function");
    }
    if (name == "Clipping" || name == "XGrid" || name == "YGrid" || name == "Visible") {
        if (const auto* numbers = std::get_if<std::vector<double>>(&value)) {
            if (numbers->size() != 1 || ((*numbers)[0] != 0 && (*numbers)[0] != 1)) {
                throw std::invalid_argument(name + " requires on, off, or scalar 0/1");
            }
            const bool enabled = (*numbers)[0] == 1;
            value = std::string(enabled ? "on" : "off");
        }
    }
    auto property = found->second.properties.find(name);
    if (property == found->second.properties.end() || property->second.index() != value.index()) {
        throw std::invalid_argument("unknown graphics property or incorrect value type");
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        if (name == "Location") {
            static const std::set<std::string> locations{"north", "south", "east", "west",
                "northeast", "northwest", "southeast", "southwest", "northoutside", "southoutside",
                "eastoutside", "westoutside", "northeastoutside", "northwestoutside",
                "southeastoutside", "southwestoutside", "best", "bestoutside"};
            if (!locations.contains(*text)) { throw std::invalid_argument("unsupported Legend Location"); }
        }
        if (name == "Orientation" && *text != "vertical" && *text != "horizontal") {
            throw std::invalid_argument("Orientation must be vertical or horizontal");
        }
        if (name == "NextPlot" && *text != "add" && *text != "replace" && *text != "replacechildren") {
            throw std::invalid_argument("NextPlot must be add, replace, or replacechildren");
        }
        if ((name == "Visible" || name == "Clipping" || name == "XGrid" || name == "YGrid") &&
            *text != "on" && *text != "off") {
            throw std::invalid_argument(name + " must be on or off");
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
        for (const auto child : children) {
            if (std::find(pending.begin(), pending.end(), child) == pending.end()) { pending.push_back(child); }
        }
        for (const auto& [candidate, node] : nodes_) {
            if (node.associatedAxes == pending[index] &&
                std::find(pending.begin(), pending.end(), candidate) == pending.end()) { pending.push_back(candidate); }
        }
    }
    for (const auto removed : pending) {
        const auto& node = nodes_.at(removed);
        if (!node.parent) { continue; }
        auto& parent = nodes_.at(*node.parent);
        std::erase(parent.children, removed);
        if (parent.currentAxes == removed) { parent.currentAxes.reset(); }
        std::erase_if(parent.labels, [removed](const auto& entry) { return entry.second == removed; });
    }
    if (currentFigure_ && std::find(pending.begin(), pending.end(), *currentFigure_) != pending.end()) {
        currentFigure_.reset();
    }
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

std::string RuntimeGraphicsHandle::serialize(bool persistentIdentity) const {
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
        {"graph", Json::parse(graph_->serialize(persistentIdentity))}}.dump(-1, ' ', true);
}

bool isRuntimeGraphicsValue(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::Object || !value.handleObject || value.objectContext ||
        (value.className != "matlab.ui.Figure" && value.className != "matlab.graphics.axis.Axes" &&
         value.className != "matlab.graphics.chart.primitive.Line" &&
         value.className != "matlab.graphics.primitive.Text" &&
         value.className != "matlab.graphics.illustration.Legend")) { return false; }
    if (value.graphicsHandle) { return true; }
    if (value.objectElements.empty()) { return runtimeShapeElementCount(value) == 0; }
    return std::any_of(value.objectElements.begin(), value.objectElements.end(),
        [](const RuntimeValue& element) { return element.graphicsHandle != nullptr; });
}

std::string serializeRuntimeGraphicsValue(const RuntimeValue& value, bool persistentIdentity) {
    if (!isRuntimeGraphicsValue(value)) { throw std::invalid_argument("expected graphics value"); }
    const auto dimensions = runtimeDimensions(value);
    const auto product = checkedRuntimeDimensionProduct(dimensions);
    if (!product || runtimeObjectElementCount(value) != *product ||
        (value.graphicsHandle && (*product != 1 || !value.objectElements.empty()))) {
        throw std::invalid_argument("graphics payload does not match its shape");
    }
    if (value.graphicsHandle) { return value.graphicsHandle->serialize(persistentIdentity); }
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
        records.push_back(Json::parse(graph->serialize(persistentIdentity)));
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
        if (handle.kind() == RuntimeGraphicsKind::Axes &&
            (name == "Title" || name == "XLabel" || name == "YLabel")) {
            return {true, makeRuntimeGraphicsValue(handle.label(name)), {}};
        }
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
        if (handle.kind() == RuntimeGraphicsKind::Axes &&
            (name == "Title" || name == "XLabel" || name == "YLabel") && value.graphicsHandle) {
            // Nested assignment copies the mutated handle back through its parent.
            if (handle.hasLabel(name, *value.graphicsHandle)) { return {true, target, {}}; }
            throw std::invalid_argument("replacing an Axes label handle is not supported");
        }
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
        ? "matlab.graphics.axis.Axes" : kind == RuntimeGraphicsKind::Text
        ? "matlab.graphics.primitive.Text" : kind == RuntimeGraphicsKind::Legend
        ? "matlab.graphics.illustration.Legend" : "matlab.graphics.chart.primitive.Line";
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
    for (auto child : node.children) {
        const bool label = std::any_of(node.labels.begin(), node.labels.end(),
            [child](const auto& entry) { return entry.second == child; });
        if (!label) { result.emplace_back(graph_, child); }
    }
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

RuntimeGraphicsHandle RuntimeGraphicsHandle::label(const std::string& role) const {
    const auto guard = graph_->lock();
    const auto& node = requireNode();
    if (node.kind != RuntimeGraphicsKind::Axes ||
        (role != "Title" && role != "XLabel" && role != "YLabel")) {
        throw std::invalid_argument("expected an Axes label role");
    }
    const auto found = node.labels.find(role);
    if (found != node.labels.end()) { return RuntimeGraphicsHandle(graph_, found->second); }
    return RuntimeGraphicsHandle(graph_, graph_->setLabel(id_, role,
        std::get<std::string>(node.properties.at(role))));
}

bool RuntimeGraphicsHandle::hasLabel(const std::string& role, const RuntimeGraphicsHandle& label) const {
    const auto guard = graph_->lock();
    const auto& node = requireNode();
    const auto found = node.labels.find(role);
    return graph_ == label.graph_ && found != node.labels.end() && found->second == label.id_;
}

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
