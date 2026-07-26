#include "mparser/c_api.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kThreadCount = 16;
constexpr std::size_t kRetainIterations = 10000;
constexpr double kTolerance = 1e-9;

constexpr char kSource[] = R"(function out = identity(value)
out = value;
end

function out = makeHandle()
out = @identity;
end

function out = invokeHandle(handle, value)
out = feval(handle, value);
end
)";

struct Handles {
    mparser_module* module = nullptr;
    mparser_session* session = nullptr;
    mparser_result* result = nullptr;
    mparser_value* value = nullptr;
    mparser_cancel_token* token = nullptr;
};

void waitForStart(const std::atomic<bool>& start) {
    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

bool scalarEquals(
    const mparser_value* value,
    double expected) {
    const double* data = nullptr;
    std::size_t count = 0;
    return value &&
           mparser_value_get_kind(value) == MPARSER_VALUE_NUMERIC &&
           mparser_value_numeric_data(value, &data, &count) ==
               MPARSER_API_STATUS_OK &&
           data && count == 1 &&
           std::abs(data[0] - expected) < kTolerance;
}

mparser_result* execute(
    mparser_module* module,
    const char* entry,
    const mparser_value* const* arguments = nullptr,
    std::size_t argumentCount = 0) {
    mparser_invocation_options options{};
    assert(MPARSER_INVOCATION_OPTIONS_INIT(&options) ==
           MPARSER_API_STATUS_OK);
    options.entry_name = entry;
    options.entry_name_size = std::strlen(entry);
    options.arguments = arguments;
    options.argument_count = argumentCount;
    options.requested_output_count = 1;
    options.has_requested_output_count = 1;
    mparser_result* result = nullptr;
    assert(mparser_module_execute(module, &options, &result) ==
           MPARSER_API_STATUS_OK);
    assert(result && mparser_result_succeeded(result) != 0);
    return result;
}

mparser_result* execute(
    mparser_session* session,
    const char* entry,
    const mparser_value* const* arguments,
    std::size_t argumentCount) {
    mparser_invocation_options options{};
    assert(MPARSER_INVOCATION_OPTIONS_INIT(&options) ==
           MPARSER_API_STATUS_OK);
    options.entry_name = entry;
    options.entry_name_size = std::strlen(entry);
    options.arguments = arguments;
    options.argument_count = argumentCount;
    options.requested_output_count = 1;
    options.has_requested_output_count = 1;
    mparser_result* result = nullptr;
    assert(mparser_session_execute(session, &options, &result) ==
           MPARSER_API_STATUS_OK);
    assert(result && mparser_result_succeeded(result) != 0);
    return result;
}

} // namespace

int main() {
    mparser_module* module = nullptr;
    assert(mparser_module_compile_utf8(
               kSource, sizeof(kSource) - 1,
               "c_api_lifecycle_stress.m",
               sizeof("c_api_lifecycle_stress.m") - 1,
               &module) == MPARSER_API_STATUS_OK);
    assert(module && mparser_module_is_valid(module) != 0);

    mparser_session* session = nullptr;
    assert(mparser_module_create_session(module, &session) ==
           MPARSER_API_STATUS_OK);
    assert(session);

    mparser_value* scalar = nullptr;
    assert(mparser_value_create_scalar(
               42.0, MPARSER_NUMERIC_DOUBLE, &scalar) ==
           MPARSER_API_STATUS_OK);
    assert(scalar);

    const mparser_value* identityArguments[]{scalar};
    mparser_result* numericResult = execute(
        module, "identity", identityArguments, 1);
    mparser_value* numericOutput = nullptr;
    assert(mparser_result_output(
               numericResult, 0, &numericOutput) ==
           MPARSER_API_STATUS_OK);
    assert(scalarEquals(numericOutput, 42.0));

    mparser_result* handleResult =
        execute(module, "makeHandle");
    mparser_value* functionHandle = nullptr;
    assert(mparser_result_output(
               handleResult, 0, &functionHandle) ==
           MPARSER_API_STATUS_OK);
    assert(functionHandle &&
           mparser_value_get_kind(functionHandle) ==
               MPARSER_VALUE_FUNCTION_HANDLE);

    mparser_cancel_token* token = nullptr;
    assert(mparser_cancel_token_create(&token) ==
           MPARSER_API_STATUS_OK);
    assert(token);

    const Handles handles{
        module, session, numericResult, functionHandle, token};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t threadIndex = 0;
         threadIndex < kThreadCount; ++threadIndex) {
        threads.emplace_back([&, threadIndex] {
            (void)threadIndex;
            waitForStart(start);
            for (std::size_t iteration = 0;
                 iteration < kRetainIterations; ++iteration) {
                mparser_module_retain(handles.module);
                mparser_session_retain(handles.session);
                mparser_result_retain(handles.result);
                mparser_value_retain(handles.value);
                mparser_cancel_token_retain(handles.token);

                if (mparser_module_is_valid(handles.module) == 0 ||
                    mparser_result_succeeded(handles.result) == 0 ||
                    mparser_value_get_kind(handles.value) !=
                        MPARSER_VALUE_FUNCTION_HANDLE ||
                    mparser_cancel_token_is_requested(handles.token) != 0) {
                    failed.store(true, std::memory_order_relaxed);
                }

                mparser_cancel_token_release(handles.token);
                mparser_value_release(handles.value);
                mparser_result_release(handles.result);
                mparser_session_release(handles.session);
                mparser_module_release(handles.module);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    assert(!failed.load(std::memory_order_relaxed));

    mparser_result_release(handleResult);
    mparser_result_release(numericResult);
    mparser_value_release(numericOutput);
    mparser_module_release(module);
    module = nullptr;

    const mparser_value* handleArguments[]{
        functionHandle, scalar};
    mparser_result* retainedResult = execute(
        session, "invokeHandle", handleArguments, 2);
    mparser_value* retainedOutput = nullptr;
    assert(mparser_result_output(
               retainedResult, 0, &retainedOutput) ==
           MPARSER_API_STATUS_OK);
    assert(scalarEquals(retainedOutput, 42.0));

    mparser_result_release(retainedResult);
    mparser_value_release(retainedOutput);
    mparser_session_release(session);
    session = nullptr;

    const auto functionText =
        mparser_value_function_text(functionHandle);
    assert(functionText.data && functionText.size != 0);

    mparser_cancel_token_release(token);
    mparser_value_release(functionHandle);
    mparser_value_release(scalar);

    std::cout << "c api lifecycle stress = "
              << kThreadCount * kRetainIterations
              << ",retained-session,retained-handle\n";
    return 0;
}
