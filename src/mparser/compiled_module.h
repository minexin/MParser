#pragma once

#include "mparser/adaptive_bytecode_vm.h"
#include "mparser/bytecode.h"
#include "mparser/function_signature.h"
#include "mparser/module_execution.h"
#include "mparser/runtime_session_state.h"
#include "mparser/source.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct CompiledModuleData;
class CompiledModuleSession;

struct CompiledFunctionInfo {
    std::string name;
    FunctionSignature signature;
    std::vector<std::string> nameValueArguments;
    SourceSpan span;
};

struct CompiledModuleCompileOptions {
    std::shared_ptr<const BuiltinRegistry> builtinRegistry;
};

class CompiledModule {
public:
    static CompiledModule compile(std::string source);
    static CompiledModule compile(
        std::string source,
        const CompiledModuleCompileOptions& options);
    static CompiledModule compile(
        std::vector<SourceUnit> sources);
    static CompiledModule compile(
        std::vector<SourceUnit> sources,
        const CompiledModuleCompileOptions& options);

    bool valid() const;
    std::string_view source() const;
    const std::vector<SourceUnit>& sources() const;
    std::string_view sourceName(size_t sourceId) const;
    std::string_view sourceName(SourceSpan span) const;
    const std::vector<Diagnostic>& diagnostics() const;
    const SemanticResult& semantic() const;
    const BytecodeProgram& bytecode() const;
    const std::vector<CompiledFunctionInfo>& functions() const;
    const CompiledFunctionInfo* findFunction(std::string_view name) const;

    std::vector<Diagnostic> validateInvocation(
        std::string_view entryFunction,
        size_t argumentCount,
        std::optional<size_t> requestedOutputCount = std::nullopt) const;
    std::vector<Diagnostic> validateInvocation(
        std::string_view entryFunction,
        const std::vector<RuntimeValue>& arguments,
        std::optional<size_t> requestedOutputCount = std::nullopt) const;
    BytecodeVmResult invoke(const BytecodeVmOptions& options = {}) const;
    ModuleInvocationResult execute(
        const ModuleInvocationRequest& request = {}) const;
    AdaptiveBytecodeVmSession createAdaptiveSession(
        const AdaptiveBytecodeVmOptions& options = {}) const;
    CompiledModuleSession createSession(
        std::shared_ptr<RuntimeSessionState> state = {}) const;

private:
    CompiledModule();
    friend class CompiledModuleSession;

    ModuleInvocationResult execute(
        const ModuleInvocationRequest& request,
        const std::shared_ptr<RuntimeSessionState>& state) const;

    std::shared_ptr<CompiledModuleData> data_;
    std::shared_ptr<RuntimeCallableContext> callableContext_;
};

class CompiledModuleSession {
public:
    BytecodeVmResult invoke(
        const BytecodeVmOptions& options = {}) const;
    ModuleInvocationResult execute(
        const ModuleInvocationRequest& request = {}) const;

    std::shared_ptr<RuntimeSessionState> state() const;
    std::vector<RuntimeVariable> globals() const;
    std::vector<RuntimePersistentVariable>
    persistentVariables() const;
    bool clearGlobal(std::string_view name);
    size_t clearFunction(std::string_view function);
    void clearGlobals();
    void reset();

private:
    friend class CompiledModule;
    CompiledModuleSession(
        CompiledModule module,
        std::shared_ptr<RuntimeSessionState> state);

    CompiledModule module_;
    std::shared_ptr<RuntimeSessionState> state_;
};

} // namespace mparser
