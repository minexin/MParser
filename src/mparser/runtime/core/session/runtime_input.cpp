#include "mparser/runtime/core/session/runtime_input.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/io/filesystem_utf8.h"

#include <chrono>
#include <exception>
#include <new>
#include <thread>

namespace mparser {

RuntimeInputResult readRuntimeInput(
    const RuntimeInputSource& source, const RuntimeInputRequest& request,
    RuntimeExecutionControl& control) {
    while (control.checkpoint()) {
        if (!source) {
            return {RuntimeInputStatus::Error, {},
                    "interactive input source is unavailable"};
        }
        RuntimeInputResult result;
        try {
            result = source(request);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            result = {RuntimeInputStatus::Error, {}, error.what()};
        } catch (...) {
            result = {RuntimeInputStatus::Error, {},
                      "interactive input callback threw an exception"};
        }
        // Cancellation wins even when a callback supplies a completed line.
        if (!control.checkpoint()) {
            break;
        }
        switch (result.status) {
        case RuntimeInputStatus::Ready:
            if (!control.observeArrayBytes(result.text.size())) {
                return {RuntimeInputStatus::Stopped, {}, {}};
            }
            if (!isValidUtf8(result.text)) {
                return {RuntimeInputStatus::Error, {},
                        "interactive input is not valid UTF-8"};
            }
            result.error.clear();
            return result;
        case RuntimeInputStatus::EndOfInput:
            return {RuntimeInputStatus::EndOfInput, {}, {}};
        case RuntimeInputStatus::Error:
            if (!isValidUtf8(result.error)) {
                result.error = "interactive input error is not valid UTF-8";
            } else if (result.error.empty()) {
                result.error = "interactive input callback failed";
            }
            result.text.clear();
            return result;
        case RuntimeInputStatus::Pending:
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            break;
        default:
            return {RuntimeInputStatus::Error, {},
                    "interactive input callback returned an invalid status"};
        }
    }
    return {RuntimeInputStatus::Stopped, {}, {}};
}

} // namespace mparser
