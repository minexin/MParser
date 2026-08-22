#include "mparser/c_api.h"
#include "mparser/embedding/c_api_test_hooks.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

using mparser::c_api_test::ExceptionKind;
using mparser::c_api_test::FaultPoint;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Handle, typename Callable>
void requireFault(
    FaultPoint point, ExceptionKind kind,
    mparser_api_status expectedStatus,
    Callable&& callable, const char* message) {
    auto* output = reinterpret_cast<Handle*>(
        static_cast<std::uintptr_t>(1));
    mparser::c_api_test::arm(point, kind);
    mparser_api_status status = MPARSER_API_STATUS_OK;
    try {
        status = std::forward<Callable>(callable)(&output);
    } catch (...) {
        mparser::c_api_test::clear();
        throw;
    }
    mparser::c_api_test::clear();
    require(status == expectedStatus, message);
    require(output == nullptr,
            "faulted C API call did not clear its output handle");
}

mparser_module* compileModule(const char* source) {
    mparser_module* module = nullptr;
    require(mparser_module_compile_utf8(
                source, std::strlen(source),
                "named_faults.m", std::strlen("named_faults.m"),
                &module) == MPARSER_API_STATUS_OK,
            "failed to compile named-fault module");
    require(module != nullptr && mparser_module_is_valid(module) != 0,
            "named-fault module is invalid");
    return module;
}

mparser_invocation_options invocationOptions(
    const char* entryName,
    const mparser_value* const* arguments = nullptr,
    std::size_t argumentCount = 0) {
    mparser_invocation_options options{};
    require(mparser_invocation_options_init(&options) ==
                MPARSER_API_STATUS_OK,
            "failed to initialize named-fault invocation");
    options.entry_name = entryName;
    options.entry_name_size = std::strlen(entryName);
    options.arguments = arguments;
    options.argument_count = argumentCount;
    options.requested_output_count = 1;
    options.has_requested_output_count = 1;
    return options;
}

mparser_api_status createDoubleScalar(
    double value, mparser_value** output) {
    constexpr std::size_t dimensions[]{1, 1};
    const mparser_numeric_buffer buffer{
        MPARSER_NUMERIC_DOUBLE, 0, &value, nullptr, 1};
    return mparser_value_create_numeric(
        dimensions, 2, &buffer, output);
}

mparser_api_status doubleData(
    const mparser_value* value,
    const double** output,
    std::size_t* count) {
    if (!output || !count) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *output = nullptr;
    *count = 0;
    mparser_numeric_buffer buffer{};
    const auto status =
        mparser_value_get_numeric_buffer(value, &buffer);
    if (status != MPARSER_API_STATUS_OK) {
        return status;
    }
    if (buffer.numeric_class != MPARSER_NUMERIC_DOUBLE ||
        buffer.is_complex != 0) {
        return MPARSER_API_STATUS_TYPE_MISMATCH;
    }
    *output = static_cast<const double*>(buffer.real_data);
    *count = buffer.element_count;
    return MPARSER_API_STATUS_OK;
}

double scalar(const mparser_result* result) {
    mparser_value* output = nullptr;
    const double* data = nullptr;
    std::size_t count = 0;
    require(result != nullptr && mparser_result_succeeded(result) != 0,
            "named-fault invocation did not succeed");
    require(mparser_result_output(result, 0, &output) ==
                MPARSER_API_STATUS_OK,
            "failed to project named-fault result");
    require(doubleData(output, &data, &count) ==
                MPARSER_API_STATUS_OK &&
                count == 1,
            "named-fault result is not scalar");
    const double value = data[0];
    mparser_value_release(output);
    return value;
}

} // namespace

int main() {
    try {
        static const char source[] =
            "function out = entry(value)\n"
            "out = value + 2;\n"
            "end\n"
            "\n"
            "function out = nextCounter()\n"
            "persistent count\n"
            "if isempty(count)\n"
            "count = 0;\n"
            "end\n"
            "count = count + 1;\n"
            "out = count;\n"
            "end\n";

        const auto compileCall = [&](mparser_module** output) {
            return mparser_module_compile_utf8(
                source, sizeof(source) - 1,
                "named_faults.m", std::strlen("named_faults.m"),
                output);
        };
        requireFault<mparser_module>(
            FaultPoint::SourceCopy, ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED, compileCall,
            "source-copy bad_alloc translation failed");
        requireFault<mparser_module>(
            FaultPoint::SourceCopy, ExceptionKind::Unknown,
            MPARSER_API_STATUS_INTERNAL_ERROR, compileCall,
            "source-copy unknown-exception translation failed");
        requireFault<mparser_module>(
            FaultPoint::ModulePublish, ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED, compileCall,
            "module-publish bad_alloc translation failed");
        requireFault<mparser_module>(
            FaultPoint::ModulePublish, ExceptionKind::Unknown,
            MPARSER_API_STATUS_INTERNAL_ERROR, compileCall,
            "module-publish unknown-exception translation failed");

        mparser_module* module = compileModule(source);

        const auto valueCall = [](mparser_value** output) {
            return createDoubleScalar(40.0, output);
        };
        requireFault<mparser_value>(
            FaultPoint::ValuePublish, ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED, valueCall,
            "value-publish bad_alloc translation failed");
        requireFault<mparser_value>(
            FaultPoint::ValuePublish, ExceptionKind::Unknown,
            MPARSER_API_STATUS_INTERNAL_ERROR, valueCall,
            "value-publish unknown-exception translation failed");

        mparser_value* argument = nullptr;
        require(valueCall(&argument) == MPARSER_API_STATUS_OK &&
                    argument != nullptr,
                "failed to create named-fault argument");
        const mparser_value* arguments[] = {argument};
        const auto entryOptions =
            invocationOptions("entry", arguments, 1);

        const auto executeCall = [&](mparser_result** output) {
            return mparser_module_execute(
                module, &entryOptions, output);
        };
        requireFault<mparser_result>(
            FaultPoint::ExecuteBeforeCore,
            ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED, executeCall,
            "pre-execution bad_alloc translation failed");
        requireFault<mparser_result>(
            FaultPoint::ExecuteAfterCore,
            ExceptionKind::Unknown,
            MPARSER_API_STATUS_INTERNAL_ERROR, executeCall,
            "post-execution unknown-exception translation failed");
        requireFault<mparser_result>(
            FaultPoint::ResultPublish,
            ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED, executeCall,
            "result-publish bad_alloc translation failed");
        requireFault<mparser_result>(
            FaultPoint::DiagnosticCopy,
            ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED, executeCall,
            "diagnostic-copy bad_alloc translation failed");

        mparser_result* result = nullptr;
        require(executeCall(&result) == MPARSER_API_STATUS_OK &&
                    std::abs(scalar(result) - 42.0) < 1e-9,
                "named-fault module stopped working after injected faults");
        requireFault<mparser_value>(
            FaultPoint::ExternalValueCaches,
            ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED,
            [&](mparser_value** output) {
                return mparser_result_output(result, 0, output);
            },
            "external-cache bad_alloc translation failed");

        const std::size_t dimensions[] = {1, 1};
        const mparser_value* cellElements[] = {argument};
        requireFault<mparser_value>(
            FaultPoint::CellCompose,
            ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED,
            [&](mparser_value** output) {
                return mparser_value_create_cell(
                    dimensions, 2, cellElements, 1, output);
            },
            "Cell composition bad_alloc translation failed");
        const mparser_named_value fields[] = {
            {"value", std::strlen("value"), argument}};
        requireFault<mparser_value>(
            FaultPoint::StructCompose,
            ExceptionKind::Unknown,
            MPARSER_API_STATUS_INTERNAL_ERROR,
            [&](mparser_value** output) {
                return mparser_value_create_struct(
                    fields, 1, output);
            },
            "Struct composition unknown-exception translation failed");

        requireFault<mparser_cancel_token>(
            FaultPoint::CancelTokenCreate,
            ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED,
            [](mparser_cancel_token** output) {
                return mparser_cancel_token_create(output);
            },
            "cancellation-token bad_alloc translation failed");
        requireFault<mparser_session>(
            FaultPoint::SessionCreate,
            ExceptionKind::Unknown,
            MPARSER_API_STATUS_INTERNAL_ERROR,
            [&](mparser_session** output) {
                return mparser_module_create_session(module, output);
            },
            "session unknown-exception translation failed");

        mparser_session* session = nullptr;
        require(mparser_module_create_session(module, &session) ==
                    MPARSER_API_STATUS_OK &&
                    session != nullptr,
                "failed to create named-fault session");
        const auto counterOptions = invocationOptions("nextCounter");
        const auto sessionCall = [&](mparser_result** output) {
            return mparser_session_execute(
                session, &counterOptions, output);
        };

        requireFault<mparser_result>(
            FaultPoint::ExecuteBeforeCore,
            ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED, sessionCall,
            "session pre-execution fault translation failed");
        mparser_result* firstCounter = nullptr;
        require(sessionCall(&firstCounter) == MPARSER_API_STATUS_OK &&
                    std::abs(scalar(firstCounter) - 1.0) < 1e-9,
                "pre-execution fault changed persistent state");
        mparser_result_release(firstCounter);

        requireFault<mparser_result>(
            FaultPoint::ExecuteAfterCore,
            ExceptionKind::BadAllocation,
            MPARSER_API_STATUS_ALLOCATION_FAILED, sessionCall,
            "session post-execution fault translation failed");
        mparser_result* thirdCounter = nullptr;
        require(sessionCall(&thirdCounter) == MPARSER_API_STATUS_OK &&
                    std::abs(scalar(thirdCounter) - 3.0) < 1e-9,
                "post-execution publication failure did not preserve committed state");
        mparser_result_release(thirdCounter);

        std::atomic<mparser_api_status> isolatedStatus{
            MPARSER_API_STATUS_INTERNAL_ERROR};
        mparser::c_api_test::arm(
            FaultPoint::ValuePublish,
            ExceptionKind::BadAllocation);
        std::thread isolated([&]() {
            mparser_value* value = nullptr;
            isolatedStatus.store(
                createDoubleScalar(1.0, &value),
                std::memory_order_relaxed);
            mparser_value_release(value);
        });
        isolated.join();
        require(isolatedStatus.load(std::memory_order_relaxed) ==
                    MPARSER_API_STATUS_OK,
                "fault injection leaked into another thread");
        mparser_value* currentThreadValue = nullptr;
        require(createDoubleScalar(
                    1.0, &currentThreadValue) ==
                    MPARSER_API_STATUS_ALLOCATION_FAILED &&
                    currentThreadValue == nullptr,
                "thread-local fault was not retained by its owner thread");
        mparser::c_api_test::clear();

        const double* argumentData = nullptr;
        std::size_t argumentCount = 0;
        require(doubleData(
                    argument, &argumentData, &argumentCount) ==
                    MPARSER_API_STATUS_OK &&
                    argumentCount == 1 &&
                    std::abs(argumentData[0] - 40.0) < 1e-9,
                "fault injection invalidated a parent value");

        mparser_session_release(session);
        mparser_result_release(result);
        mparser_value_release(argument);
        mparser_module_release(module);
        std::cout << "c api named faults = "
                     "allocation,internal,thread-local,nontransactional\n";
        return 0;
    } catch (const std::exception& exception) {
        mparser::c_api_test::clear();
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
