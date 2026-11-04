#include "mparser/runtime/core/session/runtime_event_listener_state.h"
#include "mparser/runtime/core/value/runtime_tabular.h"

#include <vector>

namespace mparser {
namespace {

// Compare actual strong owners with edges inside the listener-retained graph.
// Do not copy shared_ptrs while measuring: that would manufacture external roots.
class ListenerReachability {
public:
    void root(const std::shared_ptr<RuntimeWorkspace>& fields) { (void)workspace(fields); }

    void couple(const RuntimeWorkspace* source,const RuntimeWorkspace* listener) {
        const auto target = indices_.at(listener);
        const auto origin = indices_.find(source);
        if (origin == indices_.end()) { coupledRoots_.push_back(target); }
        else { nodes_[origin->second].children.push_back(target); }
    }

    void markExternalRoots() {
        std::vector<size_t> pending = coupledRoots_;
        for (size_t index = 0; index < nodes_.size(); ++index) {
            if (nodes_[index].owners > nodes_[index].internalOwners) { pending.push_back(index); }
        }
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            if (nodes_[index].reachable) { continue; }
            nodes_[index].reachable = true;
            for (const auto child : nodes_[index].children) { pending.push_back(child); }
        }
    }

    bool reachable(const RuntimeWorkspace* fields) const {
        const auto found = indices_.find(fields);
        return found == indices_.end() || nodes_[found->second].reachable;
    }

private:
    struct Node {
        long owners;
        long internalOwners = 0;
        std::vector<size_t> children;
        bool reachable = false;
    };
    std::map<const void*,size_t,std::less<>> indices_;
    std::vector<Node> nodes_;
    std::vector<size_t> coupledRoots_;

    template <typename Value,typename Visit>
    size_t shared(const std::shared_ptr<Value>& pointer,Visit visit) {
        const auto [found,inserted] = indices_.emplace(pointer.get(),nodes_.size());
        const auto index = found->second;
        if (inserted) { nodes_.push_back({pointer.use_count(),0,{},false}); }
        ++nodes_[index].internalOwners;
        if (inserted) { visit(index,*pointer); }
        return index;
    }

    size_t workspace(const std::shared_ptr<RuntimeWorkspace>& pointer) {
        return shared(pointer,[this](size_t index,const RuntimeWorkspace& fields) {
            values(index,fields);
        });
    }

    void values(size_t owner,const RuntimeWorkspace& fields) {
        for (const auto& [name,value] : fields) { (void)name; valueEdges(owner,value); }
    }

    void valueEdges(size_t owner,const RuntimeValue& value) {
        if (value.sharedFields) {
            const auto child = workspace(value.sharedFields);
            nodes_[owner].children.push_back(child);
        }
        if (value.functionHandle) {
            const auto child = shared(value.functionHandle,[this](size_t index,const RuntimeFunctionHandle& handle) {
                if (handle.receiver) { valueEdges(index,*handle.receiver); }
                values(index,handle.capturedVariables);
            });
            nodes_[owner].children.push_back(child);
        }
        if (value.tabularStorage) {
            const auto child = shared(value.tabularStorage,[this](size_t index,const RuntimeTabularStorage& table) {
                for (const auto& variable : table.variables) { valueEdges(index,variable.value); }
                valueEdges(index,table.rowTimes);
                valueEdges(index,table.userData);
            });
            nodes_[owner].children.push_back(child);
        }
        values(owner,value.fields);
        for (const auto& fields : value.structElements) { values(owner,fields); }
        for (const auto& item : value.cells) { valueEdges(owner,item); }
        for (const auto& item : value.objectElements) { valueEdges(owner,item); }
    }
};

} // namespace

void RuntimeEventListenerState::collectUnreachable() {
    std::scoped_lock lock(mutex);
    ListenerReachability graph;
    for (const auto& [id,record] : records) {
        (void)id;
        if (record.retainedListenerFields) { graph.root(record.retainedListenerFields); }
    }
    // These are conditional liveness edges, not additional shared_ptr owners.
    // Measure all real owners before locking any of the weak source references.
    for (const auto& [id,record] : records) {
        (void)id;
        if (record.coupled && record.retainedListenerFields) {
            if (const auto source = record.sourceFields.lock()) {
                graph.couple(source.get(),record.retainedListenerFields.get());
            }
        }
    }
    graph.markExternalRoots();
    std::erase_if(records,[&graph](const auto& item) {
        const auto& record = item.second;
        const auto source = record.sourceFields.lock();
        const bool discard = !source || record.listenerFields.expired() || !graph.reachable(source.get());
        if (discard) {
            if (const auto listener = record.listenerFields.lock()) {
                const auto valid = listener->find("__mparser_valid");
                if (valid != listener->end()) { valid->second = makeRuntimeLogicalValue(false); }
            }
        }
        return discard;
    });
}

} // namespace mparser
