#include "mparser/cli/console_input.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace mparser {
namespace {
class ConsoleInput {
public:
    RuntimeInputResult poll(const RuntimeInputRequest& request) {
        if (!prompted_) {
            std::cout << request.prompt << std::flush;
            prompted_ = true;
        }
        if (const auto newline = buffered_.find('\n'); newline != std::string::npos) {
            std::string line = buffered_.substr(0, newline);
            buffered_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            prompted_ = false;
            return {RuntimeInputStatus::Ready, std::move(line), {}};
        }
        if (ended_) {
            prompted_ = false;
            if (!buffered_.empty()) {
                std::string line;
                line.swap(buffered_);
                return {RuntimeInputStatus::Ready, std::move(line), {}};
            }
            return {};
        }
        char bytes[512];
#if defined(_WIN32)
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (!input || input == INVALID_HANDLE_VALUE) {
            return {RuntimeInputStatus::Error, {}, "standard input is unavailable"};
        }
        DWORD mode = 0;
        if (GetConsoleMode(input, &mode)) {
            DWORD available = 0;
            if (!GetNumberOfConsoleInputEvents(input, &available)) {
                return {RuntimeInputStatus::Error, {}, "cannot query console input"};
            }
            while (available-- > 0) {
                INPUT_RECORD record{};
                DWORD read = 0;
                if (!ReadConsoleInputW(input, &record, 1, &read)) {
                    return {RuntimeInputStatus::Error, {}, "cannot read console input"};
                }
                if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
                    continue;
                }
                const auto& key = record.Event.KeyEvent;
                const wchar_t character = key.uChar.UnicodeChar;
                if (character == L'\r') {
                    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                        wide_.data(), static_cast<int>(wide_.size()), nullptr, 0, nullptr, nullptr);
                    if (size == 0 && !wide_.empty()) {
                        return {RuntimeInputStatus::Error, {}, "invalid console text"};
                    }
                    std::string line(static_cast<size_t>(size), '\0');
                    if (size != 0) {
                        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_.data(),
                            static_cast<int>(wide_.size()), line.data(), size, nullptr, nullptr);
                    }
                    wide_.clear();
                    prompted_ = false;
                    std::cout << '\n' << std::flush;
                    return {RuntimeInputStatus::Ready, std::move(line), {}};
                }
                if (character == 26 && wide_.empty()) {
                    ended_ = true;
                    return {};
                }
                for (WORD repeat = 0; repeat < key.wRepeatCount; ++repeat) {
                    if (character == L'\b' && !wide_.empty()) {
                        wide_.pop_back();
                        std::cout << "\b \b" << std::flush;
                    } else if (character >= L' ') {
                        wide_.push_back(character);
                        DWORD written = 0;
                        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), &character,
                            1, &written, nullptr);
                    }
                }
            }
            return {RuntimeInputStatus::Pending, {}, {}};
        }
        DWORD available = sizeof(bytes);
        if (GetFileType(input) == FILE_TYPE_PIPE &&
            !PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) {
                return {RuntimeInputStatus::Error, {}, "cannot query input pipe"};
            }
            ended_ = true;
            return {RuntimeInputStatus::Pending, {}, {}};
        }
        if (available == 0) {
            return {RuntimeInputStatus::Pending, {}, {}};
        }
        DWORD count = 0;
        if (!ReadFile(input, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) {
                return {RuntimeInputStatus::Error, {}, "cannot read standard input"};
            }
        }
#else
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&input, 1, 0);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            return {RuntimeInputStatus::Pending, {}, {}};
        }
        if (ready < 0 || (input.revents & (POLLERR | POLLNVAL))) {
            return {RuntimeInputStatus::Error, {}, "cannot poll standard input"};
        }
        const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                return {RuntimeInputStatus::Pending, {}, {}};
            }
            return {RuntimeInputStatus::Error, {}, "cannot read standard input"};
        }
#endif
        if (count == 0) {
            ended_ = true;
        } else {
            buffered_.append(bytes, static_cast<size_t>(count));
        }
        return {RuntimeInputStatus::Pending, {}, {}};
    }
private:
    bool prompted_ = false;
    bool ended_ = false;
    std::string buffered_;
#if defined(_WIN32)
    std::wstring wide_;
#endif
};
}

RuntimeInputSource makeConsoleInputSource() {
    auto input = std::make_shared<ConsoleInput>();
    return [input](const RuntimeInputRequest& request) { return input->poll(request); };
}
RuntimeConsoleSink makeConsoleOutputSink() {
    return [](std::string_view text) {
        std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
        std::cout.flush();
        return static_cast<bool>(std::cout);
    };
}
}
