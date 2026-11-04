#pragma once

#include "mparser/runtime/core/value/runtime_value.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace mparser {

struct RuntimeEventListenerRecord {
    size_t id = 0;
    std::weak_ptr<RuntimeWorkspace> sourceFields;
    std::weak_ptr<RuntimeWorkspace> listenerFields;
    std::shared_ptr<RuntimeWorkspace> retainedListenerFields;
    std::string sourceClass;
    std::string eventName;
    std::string propertyStorageKey;
    size_t dynamicPropertyId = 0;
    bool propertyListener = false;
    bool coupled = false;
    bool callbackActive = false;
};

struct RuntimeEventListenerState {
    std::recursive_mutex mutex;
    size_t nextId = 1;
    std::map<size_t,RuntimeEventListenerRecord> records;
    std::set<const void*> destroyingSources;

    void collectUnreachable();

    void invalidate() {
        std::scoped_lock lock(mutex);
        for (auto& [id,record] : records) {
            (void)id;
            if (const auto fields = record.listenerFields.lock()) {
                (*fields)["__mparser_valid"] = makeRuntimeLogicalValue(false);
            }
        }
        records.clear();
    }
};

} // namespace mparser
