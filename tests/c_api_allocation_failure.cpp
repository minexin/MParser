#include "mparser/c_api.h"
#include "mparser/embedding/machine_protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

thread_local std::ptrdiff_t allocationsBeforeFailure = -1;

bool shouldFailAllocation() noexcept {
    if (allocationsBeforeFailure < 0) {
        return false;
    }
    if (allocationsBeforeFailure == 0) {
        allocationsBeforeFailure = -1;
        return true;
    }
    --allocationsBeforeFailure;
    return false;
}

void armAllocationFailure(
    std::ptrdiff_t allocationsBeforeFailureValue = 0) noexcept {
    allocationsBeforeFailure = allocationsBeforeFailureValue;
}

void disarmAllocationFailure() noexcept {
    allocationsBeforeFailure = -1;
}

void* allocateUnaligned(std::size_t size) {
    if (shouldFailAllocation()) {
        throw std::bad_alloc();
    }
    if (size == 0) {
        size = 1;
    }
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* allocateAligned(std::size_t size, std::size_t alignment) {
    if (shouldFailAllocation()) {
        throw std::bad_alloc();
    }
    if (size == 0) {
        size = 1;
    }
#if defined(_WIN32)
    if (void* memory = _aligned_malloc(size, alignment)) {
        return memory;
    }
#else
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment, size) == 0) {
        return memory;
    }
#endif
    throw std::bad_alloc();
}

void freeAligned(void* memory) noexcept {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Handle, typename Callable>
void requireAllocationFailure(Callable&& callable,
                              const char* operation) {
    auto* output = reinterpret_cast<Handle*>(
        static_cast<std::uintptr_t>(1));
    armAllocationFailure();
    mparser_api_status status = MPARSER_API_STATUS_INTERNAL_ERROR;
    try {
        status = std::forward<Callable>(callable)(&output);
    } catch (...) {
        disarmAllocationFailure();
        throw;
    }
    disarmAllocationFailure();
    require(status == MPARSER_API_STATUS_ALLOCATION_FAILED, operation);
    require(output == nullptr,
            "allocation failure must leave output handles null");
}

} // namespace

void* operator new(std::size_t size) {
    return allocateUnaligned(size);
}

void* operator new[](std::size_t size) {
    return allocateUnaligned(size);
}

void* operator new(std::size_t size,
                   const std::nothrow_t&) noexcept {
    try {
        return allocateUnaligned(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size,
                     const std::nothrow_t&) noexcept {
    try {
        return allocateUnaligned(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t size,
                   std::align_val_t alignment) {
    return allocateAligned(
        size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size,
                     std::align_val_t alignment) {
    return allocateAligned(
        size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size,
                   std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try {
        return allocateAligned(
            size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size,
                     std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try {
        return allocateAligned(
            size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory,
                     const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete[](void* memory,
                       const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete(void* memory,
                     std::align_val_t) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory,
                       std::align_val_t) noexcept {
    freeAligned(memory);
}

void operator delete(void* memory, std::size_t,
                     std::align_val_t) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::size_t,
                       std::align_val_t) noexcept {
    freeAligned(memory);
}

void operator delete(void* memory, std::align_val_t,
                     const std::nothrow_t&) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t,
                       const std::nothrow_t&) noexcept {
    freeAligned(memory);
}

int main() {
    try {
        constexpr char source[] =
            "function out = answer()\n"
            "out = 42;\n"
            "end\n";
        constexpr char sourceName[] = "allocation_failure.m";
        constexpr char entryName[] = "answer";

        std::FILE* machineOutput = nullptr;
#if defined(_MSC_VER)
        (void)::tmpfile_s(&machineOutput);
#else
        machineOutput = std::tmpfile();
#endif
        require(machineOutput != nullptr,
                "failed to create machine-protocol fault stream");
        mparser::ModuleInvocationResult machineResult;
        armAllocationFailure();
        const int machineExit = mparser::writeMachineResultJsonV1(
            machineOutput, machineResult, "allocation-test");
        disarmAllocationFailure();
        require(machineExit == 4,
                "machine serialization failure did not return exit 4");
        require(std::fseek(machineOutput, 0, SEEK_END) == 0,
                "failed to seek machine-protocol fault stream");
        const long machineSize = std::ftell(machineOutput);
        require(machineSize > 0,
                "machine protocol emergency document is empty");
        require(std::fseek(machineOutput, 0, SEEK_SET) == 0,
                "failed to rewind machine-protocol fault stream");
        std::string machineText(
            static_cast<std::size_t>(machineSize), '\0');
        require(std::fread(
                    machineText.data(), 1, machineText.size(),
                    machineOutput) == machineText.size(),
                "failed to read machine-protocol fault stream");
        std::fclose(machineOutput);
        std::string expectedMachineText(
            mparser::machineProtocolEmergencyJsonV1());
        expectedMachineText.push_back('\n');
        require(machineText == expectedMachineText,
                "machine protocol emergency document changed");

        requireAllocationFailure<mparser_module>(
            [&](mparser_module** output) {
                return mparser_module_compile_utf8(
                    source, sizeof(source) - 1,
                    sourceName, sizeof(sourceName) - 1,
                    output);
            },
            "module compilation did not translate std::bad_alloc");

        requireAllocationFailure<mparser_value>(
            [](mparser_value** output) {
                constexpr std::size_t dimensions[]{1, 1};
                const double value = 42.0;
                const mparser_numeric_buffer buffer{
                    MPARSER_NUMERIC_DOUBLE, 0,
                    &value, nullptr, 1};
                return mparser_value_create_numeric(
                    dimensions, 2, &buffer, output);
            },
            "value creation did not translate std::bad_alloc");

        requireAllocationFailure<mparser_cancel_token>(
            [](mparser_cancel_token** output) {
                return mparser_cancel_token_create(output);
            },
            "cancellation-token creation did not translate std::bad_alloc");

        requireAllocationFailure<mparser_debugger>(
            [](mparser_debugger** output) {
                return mparser_debugger_create(
                    [](void*, const mparser_debug_event*) -> mparser_debug_action {
                        return MPARSER_DEBUG_CONTINUE;
                    }, nullptr, output);
            },
            "debugger creation did not translate std::bad_alloc");

        mparser_system_context_options systemOptions{};
        require(mparser_system_context_options_init(&systemOptions) ==
                    MPARSER_API_STATUS_OK,
                "failed to initialize system-context options");
        constexpr char systemRoot[] = ".";
        systemOptions.root_directory = {
            systemRoot, sizeof(systemRoot) - 1};
        systemOptions.capabilities = MPARSER_SYSTEM_CAPABILITY_RANDOM;
        requireAllocationFailure<mparser_system_context>(
            [&](mparser_system_context** output) {
                return mparser_system_context_create_rooted_native(
                    &systemOptions, output);
            },
            "system-context creation did not translate std::bad_alloc");

        requireAllocationFailure<mparser_runtime>(
            [](mparser_runtime** output) {
                return mparser_runtime_create(nullptr, output);
            },
            "runtime creation did not translate std::bad_alloc");

        mparser_module* module = nullptr;
        require(mparser_module_compile_utf8(
                    source, sizeof(source) - 1,
                    sourceName, sizeof(sourceName) - 1,
                    &module) == MPARSER_API_STATUS_OK,
                "failed to compile allocation-test module");
        require(module != nullptr && mparser_module_is_valid(module) != 0,
                "allocation-test module is invalid");

        requireAllocationFailure<mparser_session>(
            [&](mparser_session** output) {
                return mparser_module_create_session(module, output);
            },
            "session creation did not translate std::bad_alloc");

        mparser_invocation_options options{};
        require(mparser_invocation_options_init(&options) ==
                    MPARSER_API_STATUS_OK,
                "failed to initialize invocation options");
        options.entry_name = entryName;
        options.entry_name_size = sizeof(entryName) - 1;
        options.requested_output_count = 1;
        options.has_requested_output_count = 1;

        requireAllocationFailure<mparser_result>(
            [&](mparser_result** output) {
                return mparser_module_execute(
                    module, &options, output);
            },
            "module execution did not translate std::bad_alloc");

        mparser_runtime* runtime = nullptr;
        require(mparser_runtime_create(nullptr, &runtime) ==
                    MPARSER_API_STATUS_OK,
                "failed to create allocation-test runtime");
        requireAllocationFailure<mparser_result>(
            [&](mparser_result** output) {
                return mparser_runtime_execute(
                    runtime, module, &options, output);
            },
            "runtime execution did not translate std::bad_alloc");

        mparser_result* runtimeResult = nullptr;
        require(mparser_runtime_execute(
                    runtime, module, &options, &runtimeResult) ==
                    MPARSER_API_STATUS_OK,
                "runtime was not reusable after allocation failure");
        require(runtimeResult != nullptr &&
                    mparser_result_succeeded(runtimeResult) != 0,
                "runtime execution after allocation failure failed");
        mparser_result_release(runtimeResult);
        mparser_runtime_release(runtime);

        mparser_result* result = nullptr;
        require(mparser_module_execute(module, &options, &result) ==
                    MPARSER_API_STATUS_OK,
                "failed to execute allocation-test module");
        require(result != nullptr && mparser_result_succeeded(result) != 0,
                "allocation-test execution failed");
        require(mparser_result_output_count(result) == 1,
                "allocation-test execution returned no output");

        requireAllocationFailure<mparser_value>(
            [&](mparser_value** output) {
                return mparser_result_output(result, 0, output);
            },
            "result projection did not translate std::bad_alloc");

        mparser_result_release(result);
        mparser_module_release(module);
        std::cout << "c api allocation failure = "
                     "protocol,compile,value,token,system-context,runtime,"
                     "session,execute,runtime-execute,output\n";
        return 0;
    } catch (const std::exception& exception) {
        disarmAllocationFailure();
        std::cerr << exception.what() << "\n";
        return 1;
    }
}
