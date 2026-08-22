#include "mparser/runtime/core/runtime_session_state.h"

#include "mparser/runtime/io/runtime_system.h"
#include "mparser/runtime/core/runtime_warning.h"

#include <utility>

namespace mparser {

RuntimeSessionState::RuntimeSessionState(
    std::shared_ptr<RuntimeSystemContext> systemContext)
    : systemContext_(systemContext
                         ? std::move(systemContext)
                         : makeIsolatedRuntimeSystemContext()),
      warningContext_(std::make_shared<RuntimeWarningContext>()) {}

std::shared_ptr<RuntimeSystemContext>
RuntimeSessionState::systemContext() const {
    return systemContext_;
}

std::shared_ptr<RuntimeWarningContext>
RuntimeSessionState::warningContext() const {
    return warningContext_;
}

RuntimeDisplayFormat RuntimeSessionState::displayFormat() const {
    std::scoped_lock lock(mutex_);
    return displayFormat_;
}

RuntimeDisplayFormat RuntimeSessionState::replaceDisplayFormat(
    RuntimeDisplayFormat format) {
    std::scoped_lock lock(mutex_);
    RuntimeDisplayFormat previous = displayFormat_;
    displayFormat_ = format;
    return previous;
}
namespace {

RuntimeValue emptyNumericMatrix() {
    RuntimeValue value;
    value.kind = RuntimeValueKind::Matrix;
    value.rows = 0;
    value.columns = 0;
    value.dimensions = {0, 0};
    value.numericClass = RuntimeNumericClass::Double;
    return value;
}

} // namespace

RuntimeValue RuntimeSessionState::declareGlobal(std::string_view name) {
    std::scoped_lock lock(mutex_);
    auto [entry, inserted] =
        globals_.try_emplace(std::string(name));
    if (inserted) {
        entry->second = emptyNumericMatrix();
    }
    return entry->second;
}

RuntimeValue RuntimeSessionState::declarePersistent(
    std::string_view function, std::string_view name) {
    return declarePersistent(0, function, name);
}

RuntimeValue RuntimeSessionState::declarePersistent(
    size_t contextIdentity, std::string_view function,
    std::string_view name) {
    std::scoped_lock lock(mutex_);
    auto& variables = persistentByFunction_[
        PersistentFunctionKey{contextIdentity, std::string(function)}];
    auto [entry, inserted] =
        variables.try_emplace(std::string(name));
    if (inserted) {
        entry->second = emptyNumericMatrix();
    }
    return entry->second;
}

std::optional<RuntimeValue> RuntimeSessionState::findGlobal(
    std::string_view name) const {
    std::scoped_lock lock(mutex_);
    const auto entry = globals_.find(std::string(name));
    return entry == globals_.end()
               ? std::nullopt
               : std::optional<RuntimeValue>(entry->second);
}

std::optional<RuntimeValue> RuntimeSessionState::findPersistent(
    std::string_view function, std::string_view name) const {
    return findPersistent(0, function, name);
}

std::optional<RuntimeValue> RuntimeSessionState::findPersistent(
    size_t contextIdentity, std::string_view function,
    std::string_view name) const {
    std::scoped_lock lock(mutex_);
    const auto owner = persistentByFunction_.find(
        PersistentFunctionKey{contextIdentity, std::string(function)});
    if (owner == persistentByFunction_.end()) {
        return std::nullopt;
    }
    const auto entry = owner->second.find(std::string(name));
    return entry == owner->second.end()
               ? std::nullopt
               : std::optional<RuntimeValue>(entry->second);
}

void RuntimeSessionState::storeGlobal(std::string name,
                                      RuntimeValue value) {
    std::scoped_lock lock(mutex_);
    globals_[std::move(name)] = std::move(value);
}

void RuntimeSessionState::storePersistent(std::string function,
                                          std::string name,
                                          RuntimeValue value) {
    storePersistent(0, std::move(function), std::move(name),
                    std::move(value));
}

void RuntimeSessionState::storePersistent(
    size_t contextIdentity, std::string function, std::string name,
    RuntimeValue value) {
    std::scoped_lock lock(mutex_);
    persistentByFunction_[PersistentFunctionKey{
        contextIdentity, std::move(function)}][std::move(name)] =
            std::move(value);
}

bool RuntimeSessionState::clearGlobal(std::string_view name) {
    std::scoped_lock lock(mutex_);
    return globals_.erase(std::string(name)) != 0;
}

bool RuntimeSessionState::clearPersistent(
    std::string_view function, std::string_view name) {
    return clearPersistent(0, function, name);
}

bool RuntimeSessionState::clearPersistent(
    size_t contextIdentity, std::string_view function,
    std::string_view name) {
    std::scoped_lock lock(mutex_);
    const auto owner = persistentByFunction_.find(
        PersistentFunctionKey{contextIdentity, std::string(function)});
    if (owner == persistentByFunction_.end()) {
        return false;
    }
    const bool removed =
        owner->second.erase(std::string(name)) != 0;
    if (owner->second.empty()) {
        persistentByFunction_.erase(owner);
    }
    return removed;
}

size_t RuntimeSessionState::clearFunction(
    std::string_view function) {
    return clearFunction(0, function);
}

size_t RuntimeSessionState::clearFunction(
    size_t contextIdentity, std::string_view function) {
    std::scoped_lock lock(mutex_);
    const auto owner = persistentByFunction_.find(
        PersistentFunctionKey{contextIdentity, std::string(function)});
    if (owner == persistentByFunction_.end()) {
        return 0;
    }
    const size_t count = owner->second.size();
    persistentByFunction_.erase(owner);
    return count;
}

void RuntimeSessionState::clearGlobals() {
    std::scoped_lock lock(mutex_);
    globals_.clear();
}

void RuntimeSessionState::reset() {
    {
        std::scoped_lock lock(mutex_);
        globals_.clear();
        persistentByFunction_.clear();
        displayFormat_ = {};
    }
    warningContext_->reset();
}

std::vector<RuntimeVariable> RuntimeSessionState::globals() const {
    std::scoped_lock lock(mutex_);
    std::vector<RuntimeVariable> result;
    result.reserve(globals_.size());
    for (const auto& [name, value] : globals_) {
        result.push_back(RuntimeVariable{name, value});
    }
    return result;
}

std::vector<RuntimePersistentVariable>
RuntimeSessionState::persistentVariables() const {
    std::scoped_lock lock(mutex_);
    std::vector<RuntimePersistentVariable> result;
    for (const auto& [owner, variables] :
         persistentByFunction_) {
        for (const auto& [name, value] : variables) {
            result.push_back(RuntimePersistentVariable{
                owner.first, owner.second, name, value});
        }
    }
    return result;
}

std::vector<RuntimePersistentVariable>
RuntimeSessionState::persistentVariables(
    size_t contextIdentity) const {
    std::scoped_lock lock(mutex_);
    std::vector<RuntimePersistentVariable> result;
    for (const auto& [owner, variables] :
         persistentByFunction_) {
        if (owner.first != contextIdentity) {
            continue;
        }
        for (const auto& [name, value] : variables) {
            result.push_back(RuntimePersistentVariable{
                owner.first, owner.second, name, value});
        }
    }
    return result;
}

} // namespace mparser
