#pragma once

#include "mparser/runtime_output.h"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

class RuntimeSystemContext;

struct RuntimePersistentVariable {
    size_t contextIdentity = 0;
    std::string function;
    std::string name;
    RuntimeValue value;
};

class RuntimeSessionState {
public:
    explicit RuntimeSessionState(
        std::shared_ptr<RuntimeSystemContext> systemContext = {});

    std::shared_ptr<RuntimeSystemContext> systemContext() const;

    RuntimeDisplayFormat displayFormat() const;
    RuntimeDisplayFormat replaceDisplayFormat(
        RuntimeDisplayFormat format);

    RuntimeValue declareGlobal(std::string_view name);
    RuntimeValue declarePersistent(std::string_view function,
                                   std::string_view name);
    RuntimeValue declarePersistent(size_t contextIdentity,
                                   std::string_view function,
                                   std::string_view name);

    std::optional<RuntimeValue> findGlobal(std::string_view name) const;
    std::optional<RuntimeValue> findPersistent(
        std::string_view function, std::string_view name) const;
    std::optional<RuntimeValue> findPersistent(
        size_t contextIdentity, std::string_view function,
        std::string_view name) const;

    void storeGlobal(std::string name, RuntimeValue value);
    void storePersistent(std::string function, std::string name,
                         RuntimeValue value);
    void storePersistent(size_t contextIdentity, std::string function,
                         std::string name, RuntimeValue value);

    bool clearGlobal(std::string_view name);
    bool clearPersistent(std::string_view function,
                         std::string_view name);
    bool clearPersistent(size_t contextIdentity,
                         std::string_view function,
                         std::string_view name);
    size_t clearFunction(std::string_view function);
    size_t clearFunction(size_t contextIdentity,
                         std::string_view function);
    void clearGlobals();
    void reset();

    std::vector<RuntimeVariable> globals() const;
    std::vector<RuntimePersistentVariable> persistentVariables() const;
    std::vector<RuntimePersistentVariable> persistentVariables(
        size_t contextIdentity) const;

private:
    using PersistentFunctionKey = std::pair<size_t, std::string>;

    mutable std::mutex mutex_;
    std::shared_ptr<RuntimeSystemContext> systemContext_;
    RuntimeDisplayFormat displayFormat_;
    std::map<std::string, RuntimeValue> globals_;
    std::map<PersistentFunctionKey,
             std::map<std::string, RuntimeValue>>
        persistentByFunction_;
};

} // namespace mparser
