#include "mparser/runtime_system.h"

#include "mparser/filesystem_utf8.h"
#include "mparser/runtime_execution_control.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <limits>
#include <numbers>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace mparser {
namespace {

constexpr size_t kMaximumProcessOutputBytes = 16U * 1024U * 1024U;

class NativeRuntimeHostFile final : public RuntimeHostFile {
public:
    NativeRuntimeHostFile(const std::filesystem::path& path,
                          const RuntimeFileOpenOptions& options)
        : readable_(options.readable), writable_(options.writable) {
#ifdef _WIN32
        binary_ = options.binary;
#endif
        std::ios::openmode mode = std::ios::openmode{};
        if (options.readable) {
            mode |= std::ios::in;
        }
        if (options.writable) {
            mode |= std::ios::out;
        }
        if (options.append) {
            mode |= std::ios::app;
        }
        if (options.truncate) {
            mode |= std::ios::trunc;
        }
        // Translation is explicit so logical scanner offsets can be mapped
        // back to physical byte positions on every standard library.
        mode |= std::ios::binary;
        stream_.open(path, mode);
    }

    bool opened() const {
        return stream_.is_open();
    }

    RuntimeSystemResult<size_t>
    write(std::string_view text) override {
        if (!writable_) {
            return RuntimeSystemResult<size_t>::failure(
                "file was not opened for writing");
        }
        if (!stream_.is_open()) {
            return RuntimeSystemResult<size_t>::failure(
                "file is closed");
        }
        std::string translated;
        std::string_view output = text;
#ifdef _WIN32
        if (!binary_) {
            translated.reserve(text.size());
            for (const char character : text) {
                if (character == '\n') {
                    translated.push_back('\r');
                }
                translated.push_back(character);
            }
            output = translated;
        }
#endif
        stream_.write(output.data(),
                      static_cast<std::streamsize>(output.size()));
        if (!stream_) {
            return RuntimeSystemResult<size_t>::failure(
                "file write failed");
        }
        lastOperation_ = Operation::Write;
        return RuntimeSystemResult<size_t>::success(text.size());
    }

    RuntimeSystemResult<RuntimeFileReadBuffer>
    readRemaining(size_t maximumBytes) override {
        if (!readable_) {
            return RuntimeSystemResult<RuntimeFileReadBuffer>::failure(
                "file was not opened for reading");
        }
        if (!stream_.is_open()) {
            return RuntimeSystemResult<RuntimeFileReadBuffer>::failure(
                "file is closed");
        }

        const std::streampos initialPosition = stream_.tellg();
        if (initialPosition == std::streampos(-1)) {
            return RuntimeSystemResult<RuntimeFileReadBuffer>::failure(
                "file position query failed before reading");
        }
        const auto restoreInitialPosition = [&] {
            stream_.clear();
            stream_.seekg(initialPosition);
            if (writable_) {
                stream_.seekp(initialPosition);
            }
            return static_cast<bool>(stream_);
        };
        size_t physicalLimit = maximumBytes;
#ifdef _WIN32
        if (!binary_) {
            physicalLimit = maximumBytes >
                                    (std::numeric_limits<size_t>::max() / 2)
                                ? std::numeric_limits<size_t>::max()
                                : maximumBytes * 2;
        }
#endif
        std::string raw;
        std::array<char, 4096> buffer{};
        while (true) {
            stream_.read(buffer.data(),
                         static_cast<std::streamsize>(buffer.size()));
            const auto count = stream_.gcount();
            if (count > 0) {
                const size_t converted = static_cast<size_t>(count);
                if (raw.size() > physicalLimit ||
                    converted > physicalLimit - raw.size()) {
                    (void)restoreInitialPosition();
                    return RuntimeSystemResult<
                        RuntimeFileReadBuffer>::failure(
                        "file read exceeds the runtime limit");
                }
                raw.append(buffer.data(), converted);
            }
            if (stream_.eof()) {
                stream_.clear();
                break;
            }
            if (!stream_) {
                (void)restoreInitialPosition();
                return RuntimeSystemResult<
                    RuntimeFileReadBuffer>::failure(
                    "file read failed");
            }
        }

        RuntimeFileReadBuffer result;
#ifdef _WIN32
        if (!binary_) {
            result.text.reserve(raw.size());
            for (size_t index = 0; index < raw.size(); ++index) {
                if (raw[index] == '\r' && index + 1 < raw.size() &&
                    raw[index + 1] == '\n') {
                    result.text.push_back('\n');
                    result.extraPhysicalByteOffsets.push_back(
                        result.text.size());
                    ++index;
                } else {
                    result.text.push_back(raw[index]);
                }
            }
        } else {
            result.text = std::move(raw);
        }
#else
        result.text = std::move(raw);
#endif
        if (result.text.size() > maximumBytes) {
            (void)restoreInitialPosition();
            return RuntimeSystemResult<RuntimeFileReadBuffer>::failure(
                "file read exceeds the runtime limit");
        }
        lastOperation_ = Operation::Read;
        return RuntimeSystemResult<RuntimeFileReadBuffer>::success(
            std::move(result));
    }

    RuntimeSystemResult<std::int64_t> position() override {
        if (!stream_.is_open()) {
            return RuntimeSystemResult<std::int64_t>::failure(
                "file is closed");
        }
        stream_.clear();
        std::streampos value = std::streampos(-1);
        if (lastOperation_ == Operation::Write && writable_) {
            value = stream_.tellp();
        } else if (readable_) {
            value = stream_.tellg();
        } else if (writable_) {
            value = stream_.tellp();
        }
        if (value == std::streampos(-1)) {
            return RuntimeSystemResult<std::int64_t>::failure(
                "file position query failed");
        }
        const auto offset = static_cast<std::streamoff>(value);
        if (offset < 0 ||
            static_cast<std::uintmax_t>(offset) >
                static_cast<std::uintmax_t>(
                    std::numeric_limits<std::int64_t>::max())) {
            return RuntimeSystemResult<std::int64_t>::failure(
                "file position is outside the supported range");
        }
        return RuntimeSystemResult<std::int64_t>::success(
            static_cast<std::int64_t>(offset));
    }

    RuntimeSystemStatus seek(
        std::int64_t offset, RuntimeFileSeekOrigin origin) override {
        if (!stream_.is_open()) {
            return RuntimeSystemStatus::failure("file is closed");
        }

        const auto original = position();
        if (!original.succeeded) {
            return RuntimeSystemStatus::failure(original.error);
        }
        const auto setAbsolutePosition = [this](std::int64_t value) {
            stream_.clear();
            const auto converted = static_cast<std::streamoff>(value);
            if (readable_) {
                stream_.seekg(converted, std::ios::beg);
            }
            if (writable_) {
                stream_.seekp(converted, std::ios::beg);
            }
            return static_cast<bool>(stream_);
        };
        const auto failureWithOriginalPosition =
            [&](std::string message) {
                stream_.clear();
                if (!setAbsolutePosition(original.value)) {
                    message += "; original position could not be restored";
                    stream_.clear();
                }
                return RuntimeSystemStatus::failure(std::move(message));
            };

        std::int64_t base = 0;
        if (origin == RuntimeFileSeekOrigin::Current) {
            base = original.value;
        } else if (origin == RuntimeFileSeekOrigin::End) {
            stream_.clear();
            if (readable_) {
                stream_.seekg(0, std::ios::end);
                const auto end = stream_.tellg();
                if (end != std::streampos(-1)) {
                    const auto converted = static_cast<std::streamoff>(end);
                    if (converted >= 0 &&
                        static_cast<std::uintmax_t>(converted) <=
                            static_cast<std::uintmax_t>(
                                std::numeric_limits<std::int64_t>::max())) {
                        base = static_cast<std::int64_t>(converted);
                    } else {
                        return failureWithOriginalPosition(
                            "file size is outside the supported range");
                    }
                } else {
                    return failureWithOriginalPosition(
                        "file end position query failed");
                }
            } else {
                stream_.seekp(0, std::ios::end);
                const auto end = stream_.tellp();
                if (end == std::streampos(-1)) {
                    return failureWithOriginalPosition(
                        "file end position query failed");
                }
                const auto converted = static_cast<std::streamoff>(end);
                if (converted < 0 ||
                    static_cast<std::uintmax_t>(converted) >
                        static_cast<std::uintmax_t>(
                            std::numeric_limits<std::int64_t>::max())) {
                    return failureWithOriginalPosition(
                        "file size is outside the supported range");
                }
                base = static_cast<std::int64_t>(converted);
            }
            if (!setAbsolutePosition(original.value)) {
                stream_.clear();
                return RuntimeSystemStatus::failure(
                    "file position restore failed");
            }
        }

        if ((offset > 0 &&
             base > std::numeric_limits<std::int64_t>::max() - offset) ||
            (offset < 0 &&
             base < std::numeric_limits<std::int64_t>::min() - offset)) {
            return RuntimeSystemStatus::failure(
                "file seek target is outside the supported range");
        }
        const std::int64_t target = base + offset;
        if (target < 0 ||
            static_cast<std::uintmax_t>(target) >
                static_cast<std::uintmax_t>(
                    std::numeric_limits<std::streamoff>::max())) {
            return RuntimeSystemStatus::failure(
                "file seek target is outside the supported range");
        }

        if (!setAbsolutePosition(target)) {
            stream_.clear();
            (void)setAbsolutePosition(original.value);
            return RuntimeSystemStatus::failure("file seek failed");
        }
        lastOperation_ = Operation::None;
        return RuntimeSystemStatus::success();
    }

    RuntimeSystemStatus close() override {
        if (!stream_.is_open()) {
            return RuntimeSystemStatus::failure("file is already closed");
        }
        stream_.close();
        return stream_.fail()
                   ? RuntimeSystemStatus::failure("file close failed")
                   : RuntimeSystemStatus::success();
    }

private:
    enum class Operation {
        None,
        Read,
        Write,
    };

    std::fstream stream_;
    bool readable_ = false;
    bool writable_ = false;
#ifdef _WIN32
    bool binary_ = true;
#endif
    Operation lastOperation_ = Operation::None;
};

std::string errorMessage(std::string_view operation,
                         const std::error_code& error) {
    return std::string(operation) + " failed: " + error.message();
}

RuntimeSystemResult<RuntimeCalendarTime>
calendarTime(std::chrono::system_clock::time_point value) {
    const auto seconds =
        std::chrono::time_point_cast<std::chrono::seconds>(value);
    const auto fraction =
        std::chrono::duration<double>(value - seconds).count();
    const std::time_t raw = std::chrono::system_clock::to_time_t(value);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &raw) != 0) {
        return RuntimeSystemResult<RuntimeCalendarTime>::failure(
            "local time conversion failed");
    }
#else
    if (!localtime_r(&raw, &local)) {
        return RuntimeSystemResult<RuntimeCalendarTime>::failure(
            "local time conversion failed");
    }
#endif
    return RuntimeSystemResult<RuntimeCalendarTime>::success({
        local.tm_year + 1900,
        local.tm_mon + 1,
        local.tm_mday,
        local.tm_hour,
        local.tm_min,
        static_cast<double>(local.tm_sec) + fraction,
    });
}

std::string formatDirectoryDate(
    const std::filesystem::file_time_type& value) {
    try {
        const auto systemValue = std::chrono::time_point_cast<
            std::chrono::system_clock::duration>(
            value - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
        const auto converted = calendarTime(systemValue);
        if (!converted.succeeded) {
            return {};
        }
        static constexpr std::array<std::string_view, 12> months = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun",
            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
        };
        const auto& time = converted.value;
        if (time.month < 1 || time.month > 12) {
            return {};
        }
        std::ostringstream output;
        output << std::setfill('0') << std::setw(2) << time.day << '-'
               << months[static_cast<size_t>(time.month - 1)] << '-'
               << std::setw(4) << time.year << ' ' << std::setw(2)
               << time.hour << ':' << std::setw(2) << time.minute << ':'
               << std::setw(2)
               << static_cast<int>(std::floor(time.second));
        return output.str();
    } catch (...) {
        return {};
    }
}

class NativeRuntimeHostAdapter final : public RuntimeHostAdapter {
public:
    RuntimeSystemResult<std::filesystem::path>
    processCurrentDirectory() const override {
        std::error_code error;
        auto result = std::filesystem::current_path(error);
        return error
                   ? RuntimeSystemResult<std::filesystem::path>::failure(
                         errorMessage("current directory query", error))
                   : RuntimeSystemResult<std::filesystem::path>::success(
                         std::move(result));
    }

    RuntimeSystemResult<std::filesystem::path>
    temporaryDirectory() const override {
        std::error_code error;
        auto result = std::filesystem::temp_directory_path(error);
        return error
                   ? RuntimeSystemResult<std::filesystem::path>::failure(
                         errorMessage("temporary directory query", error))
                   : RuntimeSystemResult<std::filesystem::path>::success(
                         std::move(result));
    }

    RuntimeSystemResult<std::filesystem::path>
    normalizeDirectory(const std::filesystem::path& base,
                       const std::filesystem::path& candidate) const override {
        std::filesystem::path combined = candidate;
        if (combined.is_relative()) {
            combined = base / combined;
        }
        std::error_code error;
        auto normalized = std::filesystem::weakly_canonical(combined, error);
        if (error) {
            return RuntimeSystemResult<std::filesystem::path>::failure(
                errorMessage("directory normalization", error));
        }
        const bool exists = std::filesystem::is_directory(normalized, error);
        if (error) {
            return RuntimeSystemResult<std::filesystem::path>::failure(
                errorMessage("directory query", error));
        }
        if (!exists) {
            return RuntimeSystemResult<std::filesystem::path>::failure(
                "directory does not exist: " +
                pathToNativeUtf8(normalized));
        }
        return RuntimeSystemResult<std::filesystem::path>::success(
            std::move(normalized));
    }

    RuntimeSystemResult<std::vector<RuntimeDirectoryEntry>>
    listDirectory(const std::filesystem::path& path) const override {
        std::error_code error;
        std::filesystem::directory_iterator iterator(path, error);
        if (error) {
            return RuntimeSystemResult<
                std::vector<RuntimeDirectoryEntry>>::failure(
                errorMessage("directory listing", error));
        }

        std::vector<RuntimeDirectoryEntry> result;
        for (const auto& entry : iterator) {
            RuntimeDirectoryEntry projected;
            projected.name = pathToUtf8(entry.path().filename());
            projected.folder =
                pathToNativeUtf8(entry.path().parent_path());
            projected.directory = entry.is_directory(error);
            if (error) {
                return RuntimeSystemResult<
                    std::vector<RuntimeDirectoryEntry>>::failure(
                    errorMessage("directory entry type query", error));
            }
            if (!projected.directory && entry.is_regular_file(error)) {
                projected.bytes = entry.file_size(error);
                if (error) {
                    return RuntimeSystemResult<
                        std::vector<RuntimeDirectoryEntry>>::failure(
                        errorMessage("directory entry size query", error));
                }
            }
            const auto modified = entry.last_write_time(error);
            if (!error) {
                projected.date = formatDirectoryDate(modified);
            } else {
                error.clear();
            }
            result.push_back(std::move(projected));
        }
        std::sort(result.begin(), result.end(),
                  [](const RuntimeDirectoryEntry& left,
                     const RuntimeDirectoryEntry& right) {
                      return left.name < right.name;
                  });
        return RuntimeSystemResult<
            std::vector<RuntimeDirectoryEntry>>::success(
            std::move(result));
    }

    RuntimeSystemResult<bool>
    regularFileExists(const std::filesystem::path& path) const override {
        std::error_code error;
        const bool result = std::filesystem::is_regular_file(path, error);
        if (error == std::errc::no_such_file_or_directory ||
            error == std::errc::not_a_directory) {
            return RuntimeSystemResult<bool>::success(false);
        }
        return error
                   ? RuntimeSystemResult<bool>::failure(
                         errorMessage("file query", error))
                   : RuntimeSystemResult<bool>::success(result);
    }

    RuntimeSystemResult<bool>
    directoryExists(const std::filesystem::path& path) const override {
        std::error_code error;
        const bool result = std::filesystem::is_directory(path, error);
        if (error == std::errc::no_such_file_or_directory ||
            error == std::errc::not_a_directory) {
            return RuntimeSystemResult<bool>::success(false);
        }
        return error
                   ? RuntimeSystemResult<bool>::failure(
                         errorMessage("directory query", error))
                   : RuntimeSystemResult<bool>::success(result);
    }

    RuntimeSystemResult<std::shared_ptr<RuntimeHostFile>>
    openFile(const std::filesystem::path& path,
             const RuntimeFileOpenOptions& options) const override {
        auto file = std::make_shared<NativeRuntimeHostFile>(path, options);
        if (!file->opened()) {
            return RuntimeSystemResult<
                std::shared_ptr<RuntimeHostFile>>::failure(
                "could not open file: " + pathToNativeUtf8(path));
        }
        return RuntimeSystemResult<
            std::shared_ptr<RuntimeHostFile>>::success(std::move(file));
    }

    RuntimeSystemResult<std::optional<std::string>>
    environment(std::string_view name) const override {
        if (name.find('\0') != std::string_view::npos) {
            return RuntimeSystemResult<
                std::optional<std::string>>::failure(
                "environment variable name contains a null byte");
        }
        const std::string copiedName(name);
#ifdef _WIN32
        char* rawValue = nullptr;
        size_t rawSize = 0;
        const errno_t error =
            _dupenv_s(&rawValue, &rawSize, copiedName.c_str());
        if (error != 0) {
            return RuntimeSystemResult<
                std::optional<std::string>>::failure(
                "environment variable query failed");
        }
        std::optional<std::string> value;
        if (rawValue) {
            value = std::string(rawValue);
            std::free(rawValue);
        }
        return RuntimeSystemResult<std::optional<std::string>>::success(
            std::move(value));
#else
        const char* value = std::getenv(copiedName.c_str());
        return RuntimeSystemResult<std::optional<std::string>>::success(
            value ? std::optional<std::string>(value) : std::nullopt);
#endif
    }

    RuntimeSystemResult<RuntimeCalendarTime>
    localCalendarTime() const override {
        return calendarTime(std::chrono::system_clock::now());
    }

    RuntimeSystemStatus sleepFor(
        std::chrono::nanoseconds duration) const override {
        std::this_thread::sleep_for(duration);
        return RuntimeSystemStatus::success();
    }

    RuntimeSystemResult<RuntimeProcessOutput>
    executeProcess(std::string_view command) const override {
        if (command.find('\0') != std::string_view::npos) {
            return RuntimeSystemResult<RuntimeProcessOutput>::failure(
                "process command contains a null byte");
        }
        std::string shellCommand(command);
        shellCommand += " 2>&1";
#ifdef _WIN32
        FILE* pipe = _popen(shellCommand.c_str(), "r");
#else
        FILE* pipe = popen(shellCommand.c_str(), "r");
#endif
        if (!pipe) {
            return RuntimeSystemResult<RuntimeProcessOutput>::failure(
                "failed to start process command");
        }

        RuntimeProcessOutput result;
        std::array<char, 4096> buffer{};
        bool readFailed = false;
        while (true) {
            const size_t read =
                std::fread(buffer.data(), 1, buffer.size(), pipe);
            if (read != 0) {
                if (result.output.size() >
                    kMaximumProcessOutputBytes -
                        std::min(kMaximumProcessOutputBytes, read)) {
#ifdef _WIN32
                    (void)_pclose(pipe);
#else
                    (void)pclose(pipe);
#endif
                    return RuntimeSystemResult<RuntimeProcessOutput>::failure(
                        "process output exceeds the 16 MiB limit");
                }
                result.output.append(buffer.data(), read);
            }
            if (read < buffer.size()) {
                if (std::feof(pipe)) {
                    break;
                }
                if (std::ferror(pipe)) {
                    readFailed = true;
                    break;
                }
            }
        }
#ifdef _WIN32
        result.status = _pclose(pipe);
#else
        const int rawStatus = pclose(pipe);
        if (rawStatus == -1) {
            result.status = -1;
        } else if (WIFEXITED(rawStatus)) {
            result.status = WEXITSTATUS(rawStatus);
        } else if (WIFSIGNALED(rawStatus)) {
            result.status = 128 + WTERMSIG(rawStatus);
        } else {
            result.status = rawStatus;
        }
#endif
        if (readFailed) {
            return RuntimeSystemResult<RuntimeProcessOutput>::failure(
                "failed to read process output");
        }
        return RuntimeSystemResult<RuntimeProcessOutput>::success(
            std::move(result));
    }
};

RuntimeSystemCapability nativeCapabilities() {
    return RuntimeSystemCapability::CurrentDirectory |
           RuntimeSystemCapability::SearchPaths |
           RuntimeSystemCapability::EnvironmentRead |
           RuntimeSystemCapability::FileSystemRead |
           RuntimeSystemCapability::FileSystemWrite |
           RuntimeSystemCapability::Process |
           RuntimeSystemCapability::Clock |
           RuntimeSystemCapability::Sleep |
           RuntimeSystemCapability::Random |
           RuntimeSystemCapability::DynamicEvaluation;
}

double nextUniform(std::mt19937_64& engine) {
    constexpr double scale = 1.0 / 9007199254740992.0;
    return static_cast<double>(engine() >> 11U) * scale;
}

} // namespace

RuntimeSystemCapability operator|(RuntimeSystemCapability left,
                                  RuntimeSystemCapability right) {
    return static_cast<RuntimeSystemCapability>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

RuntimeSystemCapability operator&(RuntimeSystemCapability left,
                                  RuntimeSystemCapability right) {
    return static_cast<RuntimeSystemCapability>(
        static_cast<std::uint32_t>(left) &
        static_cast<std::uint32_t>(right));
}

bool hasRuntimeSystemCapability(RuntimeSystemCapability value,
                                RuntimeSystemCapability expected) {
    return (value & expected) == expected;
}

RuntimeSystemStatus RuntimeSystemStatus::success() {
    return RuntimeSystemStatus{true, {}};
}

RuntimeSystemStatus RuntimeSystemStatus::failure(std::string message) {
    return RuntimeSystemStatus{false, std::move(message)};
}

std::shared_ptr<RuntimeHostAdapter> makeNativeRuntimeHostAdapter() {
    return std::make_shared<NativeRuntimeHostAdapter>();
}

RuntimeSystemContext::RuntimeSystemContext(
    RuntimeSystemContextOptions options)
    : capabilities_(options.capabilities),
      hostAdapter_(options.hostAdapter
                       ? std::move(options.hostAdapter)
                       : makeNativeRuntimeHostAdapter()),
      searchPaths_(std::move(options.searchPaths)),
      maximumOpenFiles_(options.maximumOpenFiles),
      maximumFileReadBytes_(options.maximumFileReadBytes),
      randomSeed_(options.randomSeed),
      randomEngine_(options.randomSeed) {
    if (options.currentDirectory) {
        currentDirectory_ = std::move(*options.currentDirectory);
    } else {
        auto current = hostAdapter_->processCurrentDirectory();
        currentDirectory_ = current.succeeded
                                ? std::move(current.value)
                                : std::filesystem::path(".");
    }
}

RuntimeSystemContext::~RuntimeSystemContext() {
    try {
        (void)closeAllFiles();
    } catch (...) {
    }
}

RuntimeSystemCapability
RuntimeSystemContext::capabilities() const noexcept {
    return capabilities_;
}

bool RuntimeSystemContext::hasCapability(
    RuntimeSystemCapability capability) const noexcept {
    return hasRuntimeSystemCapability(capabilities_, capability);
}

RuntimeSystemStatus RuntimeSystemContext::require(
    RuntimeSystemCapability capability,
    std::string_view operation) const {
    return hasCapability(capability)
               ? RuntimeSystemStatus::success()
               : RuntimeSystemStatus::failure(
                     std::string(operation) +
                     " is disabled by the runtime system capability policy");
}

RuntimeSystemResult<std::filesystem::path>
RuntimeSystemContext::currentDirectory() const {
    const auto permission = require(
        RuntimeSystemCapability::CurrentDirectory,
        "current-directory access");
    if (!permission.succeeded) {
        return RuntimeSystemResult<std::filesystem::path>::failure(
            permission.error);
    }
    std::lock_guard lock(mutex_);
    return RuntimeSystemResult<std::filesystem::path>::success(
        currentDirectory_);
}

RuntimeSystemResult<std::filesystem::path>
RuntimeSystemContext::normalizeDirectory(
    const std::filesystem::path& path) const {
    std::filesystem::path base;
    {
        std::lock_guard lock(mutex_);
        base = currentDirectory_;
    }
    return hostAdapter_->normalizeDirectory(base, path);
}

RuntimeSystemStatus RuntimeSystemContext::changeCurrentDirectory(
    const std::filesystem::path& path) {
    const auto permission = require(
        RuntimeSystemCapability::CurrentDirectory,
        "current-directory mutation");
    if (!permission.succeeded) {
        return permission;
    }
    auto normalized = normalizeDirectory(path);
    if (!normalized.succeeded) {
        return RuntimeSystemStatus::failure(
            std::move(normalized.error));
    }
    std::lock_guard lock(mutex_);
    currentDirectory_ = std::move(normalized.value);
    return RuntimeSystemStatus::success();
}

RuntimeSystemResult<std::filesystem::path>
RuntimeSystemContext::temporaryDirectory() const {
    const auto permission = require(
        RuntimeSystemCapability::CurrentDirectory,
        "temporary-directory access");
    if (!permission.succeeded) {
        return RuntimeSystemResult<std::filesystem::path>::failure(
            permission.error);
    }
    return hostAdapter_->temporaryDirectory();
}

RuntimeSystemResult<std::vector<std::filesystem::path>>
RuntimeSystemContext::searchPaths() const {
    const auto permission = require(
        RuntimeSystemCapability::SearchPaths,
        "search-path access");
    if (!permission.succeeded) {
        return RuntimeSystemResult<
            std::vector<std::filesystem::path>>::failure(
            permission.error);
    }
    std::lock_guard lock(mutex_);
    return RuntimeSystemResult<
        std::vector<std::filesystem::path>>::success(searchPaths_);
}

RuntimeSystemStatus RuntimeSystemContext::setSearchPaths(
    std::vector<std::filesystem::path> paths) {
    const auto permission = require(
        RuntimeSystemCapability::SearchPaths,
        "search-path mutation");
    if (!permission.succeeded) {
        return permission;
    }
    std::vector<std::filesystem::path> normalized;
    normalized.reserve(paths.size());
    for (const auto& path : paths) {
        auto value = normalizeDirectory(path);
        if (!value.succeeded) {
            return RuntimeSystemStatus::failure(std::move(value.error));
        }
        if (std::find(normalized.begin(), normalized.end(), value.value) ==
            normalized.end()) {
            normalized.push_back(std::move(value.value));
        }
    }
    std::lock_guard lock(mutex_);
    searchPaths_ = std::move(normalized);
    return RuntimeSystemStatus::success();
}

RuntimeSystemStatus RuntimeSystemContext::addSearchPaths(
    const std::vector<std::filesystem::path>& paths,
    bool prepend) {
    auto current = searchPaths();
    if (!current.succeeded) {
        return RuntimeSystemStatus::failure(std::move(current.error));
    }
    std::vector<std::filesystem::path> additions;
    additions.reserve(paths.size());
    for (const auto& path : paths) {
        auto normalized = normalizeDirectory(path);
        if (!normalized.succeeded) {
            return RuntimeSystemStatus::failure(
                std::move(normalized.error));
        }
        if (std::find(additions.begin(), additions.end(),
                      normalized.value) == additions.end()) {
            additions.push_back(std::move(normalized.value));
        }
    }
    auto result = std::move(current.value);
    for (const auto& addition : additions) {
        result.erase(std::remove(result.begin(), result.end(), addition),
                     result.end());
    }
    if (prepend) {
        result.insert(result.begin(), additions.begin(), additions.end());
    } else {
        result.insert(result.end(), additions.begin(), additions.end());
    }
    return setSearchPaths(std::move(result));
}

RuntimeSystemStatus RuntimeSystemContext::removeSearchPaths(
    const std::vector<std::filesystem::path>& paths) {
    auto current = searchPaths();
    if (!current.succeeded) {
        return RuntimeSystemStatus::failure(std::move(current.error));
    }
    for (const auto& path : paths) {
        auto normalized = normalizeDirectory(path);
        if (!normalized.succeeded) {
            return RuntimeSystemStatus::failure(
                std::move(normalized.error));
        }
        current.value.erase(
            std::remove(current.value.begin(), current.value.end(),
                        normalized.value),
            current.value.end());
    }
    return setSearchPaths(std::move(current.value));
}

RuntimeSystemResult<std::vector<RuntimeDirectoryEntry>>
RuntimeSystemContext::listDirectory(
    const std::filesystem::path& path) const {
    const auto permission = require(
        RuntimeSystemCapability::FileSystemRead,
        "filesystem read");
    if (!permission.succeeded) {
        return RuntimeSystemResult<
            std::vector<RuntimeDirectoryEntry>>::failure(
            permission.error);
    }
    std::filesystem::path base;
    {
        std::lock_guard lock(mutex_);
        base = currentDirectory_;
    }
    const auto target = path.empty() || path.is_absolute()
                            ? (path.empty() ? base : path)
                            : base / path;
    return hostAdapter_->listDirectory(target);
}

RuntimeSystemResult<bool> RuntimeSystemContext::regularFileExists(
    const std::filesystem::path& path) const {
    const auto permission = require(
        RuntimeSystemCapability::FileSystemRead,
        "filesystem read");
    if (!permission.succeeded) {
        return RuntimeSystemResult<bool>::failure(permission.error);
    }
    std::filesystem::path base;
    {
        std::lock_guard lock(mutex_);
        base = currentDirectory_;
    }
    return hostAdapter_->regularFileExists(
        path.is_absolute() ? path : base / path);
}

RuntimeSystemResult<bool> RuntimeSystemContext::directoryExists(
    const std::filesystem::path& path) const {
    const auto permission = require(
        RuntimeSystemCapability::FileSystemRead,
        "filesystem read");
    if (!permission.succeeded) {
        return RuntimeSystemResult<bool>::failure(permission.error);
    }
    std::filesystem::path base;
    {
        std::lock_guard lock(mutex_);
        base = currentDirectory_;
    }
    return hostAdapter_->directoryExists(
        path.is_absolute() ? path : base / path);
}

RuntimeSystemResult<int> RuntimeSystemContext::openFile(
    const std::filesystem::path& path,
    const RuntimeFileOpenOptions& options) {
    if (!options.readable && !options.writable) {
        return RuntimeSystemResult<int>::failure(
            "file mode must permit reading or writing");
    }
    if (options.readable) {
        const auto permission = require(
            RuntimeSystemCapability::FileSystemRead, "filesystem read");
        if (!permission.succeeded) {
            return RuntimeSystemResult<int>::failure(permission.error);
        }
    }
    if (options.writable) {
        const auto permission = require(
            RuntimeSystemCapability::FileSystemWrite, "filesystem write");
        if (!permission.succeeded) {
            return RuntimeSystemResult<int>::failure(permission.error);
        }
    }

    std::lock_guard lock(mutex_);
    if (openFiles_.size() >= maximumOpenFiles_) {
        return RuntimeSystemResult<int>::failure(
            "open file count exceeds the runtime limit");
    }
    std::filesystem::path target =
        path.is_absolute() ? path : currentDirectory_ / path;
    target = target.lexically_normal();
    auto opened = hostAdapter_->openFile(target, options);
    if (!opened.succeeded) {
        return RuntimeSystemResult<int>::failure(std::move(opened.error));
    }

    int identifier = nextFileIdentifier_;
    while (identifier >= 3 && openFiles_.contains(identifier)) {
        if (identifier == std::numeric_limits<int>::max()) {
            identifier = 3;
        } else {
            ++identifier;
        }
        if (identifier == nextFileIdentifier_) {
            return RuntimeSystemResult<int>::failure(
                "no file identifiers are available");
        }
    }
    nextFileIdentifier_ = identifier == std::numeric_limits<int>::max()
                              ? 3
                              : identifier + 1;
    OpenFileEntry entry;
    entry.info = RuntimeOpenFileInfo{
        identifier, path, std::move(target), options};
    entry.file = std::move(opened.value);
    openFiles_.emplace(identifier, std::move(entry));
    return RuntimeSystemResult<int>::success(identifier);
}

RuntimeSystemResult<size_t> RuntimeSystemContext::writeFile(
    int identifier, std::string_view text) {
    const auto permission = require(
        RuntimeSystemCapability::FileSystemWrite, "filesystem write");
    if (!permission.succeeded) {
        return RuntimeSystemResult<size_t>::failure(permission.error);
    }
    std::lock_guard lock(mutex_);
    const auto found = openFiles_.find(identifier);
    if (found == openFiles_.end()) {
        return RuntimeSystemResult<size_t>::failure(
            "invalid file identifier");
    }
    if (!found->second.info.options.writable) {
        return RuntimeSystemResult<size_t>::failure(
            "file was not opened for writing");
    }
    if (found->second.lastOperation ==
        OpenFileEntry::LastOperation::Read) {
        return RuntimeSystemResult<size_t>::failure(
            "fseek or frewind is required between reading and writing an "
            "update stream");
    }
    auto result = found->second.file->write(text);
    if (result.succeeded) {
        found->second.lastOperation =
            OpenFileEntry::LastOperation::Write;
    }
    return result;
}

RuntimeSystemResult<std::string>
RuntimeSystemContext::readFileRemaining(int identifier) {
    const auto permission = require(
        RuntimeSystemCapability::FileSystemRead, "filesystem read");
    if (!permission.succeeded) {
        return RuntimeSystemResult<std::string>::failure(permission.error);
    }
    std::lock_guard lock(mutex_);
    const auto found = openFiles_.find(identifier);
    if (found == openFiles_.end()) {
        return RuntimeSystemResult<std::string>::failure(
            "invalid file identifier");
    }
    if (!found->second.info.options.readable) {
        return RuntimeSystemResult<std::string>::failure(
            "file was not opened for reading");
    }
    if (found->second.lastOperation ==
        OpenFileEntry::LastOperation::Write) {
        return RuntimeSystemResult<std::string>::failure(
            "fseek or frewind is required between writing and reading an "
            "update stream");
    }
    if (found->second.unreadInput.size() > maximumFileReadBytes_) {
        return RuntimeSystemResult<std::string>::failure(
            "buffered file input exceeds the runtime limit");
    }
    auto remainder = found->second.file->readRemaining(
        maximumFileReadBytes_ - found->second.unreadInput.size());
    if (!remainder.succeeded) {
        return RuntimeSystemResult<std::string>::failure(
            std::move(remainder.error));
    }
    std::string result = std::move(found->second.unreadInput);
    std::vector<size_t> physicalOffsets =
        std::move(found->second.unreadExtraPhysicalByteOffsets);
    found->second.unreadInput.clear();
    found->second.unreadExtraPhysicalByteOffsets.clear();
    const size_t prefixSize = result.size();
    result += remainder.value.text;
    physicalOffsets.reserve(
        physicalOffsets.size() +
        remainder.value.extraPhysicalByteOffsets.size());
    for (const size_t offset :
         remainder.value.extraPhysicalByteOffsets) {
        physicalOffsets.push_back(prefixSize + offset);
    }
    found->second.pendingReadSize = result.size();
    found->second.pendingExtraPhysicalByteOffsets =
        std::move(physicalOffsets);
    found->second.lastOperation =
        OpenFileEntry::LastOperation::Read;
    return RuntimeSystemResult<std::string>::success(std::move(result));
}

RuntimeSystemStatus RuntimeSystemContext::restoreUnreadFileData(
    int identifier, std::string text) {
    if (text.size() > maximumFileReadBytes_) {
        return RuntimeSystemStatus::failure(
            "buffered file input exceeds the runtime limit");
    }
    std::lock_guard lock(mutex_);
    const auto found = openFiles_.find(identifier);
    if (found == openFiles_.end()) {
        return RuntimeSystemStatus::failure("invalid file identifier");
    }
    if (text.size() > found->second.pendingReadSize) {
        return RuntimeSystemStatus::failure(
            "unread data exceeds the preceding file read");
    }
    const size_t consumed = found->second.pendingReadSize - text.size();
    const auto firstUnread = std::upper_bound(
        found->second.pendingExtraPhysicalByteOffsets.begin(),
        found->second.pendingExtraPhysicalByteOffsets.end(), consumed);
    found->second.unreadExtraPhysicalByteOffsets.clear();
    found->second.unreadExtraPhysicalByteOffsets.reserve(
        static_cast<size_t>(
            found->second.pendingExtraPhysicalByteOffsets.end() -
            firstUnread));
    for (auto offset = firstUnread;
         offset !=
         found->second.pendingExtraPhysicalByteOffsets.end(); ++offset) {
        found->second.unreadExtraPhysicalByteOffsets.push_back(
            *offset - consumed);
    }
    found->second.unreadInput = std::move(text);
    found->second.pendingReadSize = 0;
    found->second.pendingExtraPhysicalByteOffsets.clear();
    return RuntimeSystemStatus::success();
}

RuntimeSystemResult<std::int64_t>
RuntimeSystemContext::filePosition(int identifier) {
    std::lock_guard lock(mutex_);
    const auto found = openFiles_.find(identifier);
    if (found == openFiles_.end()) {
        return RuntimeSystemResult<std::int64_t>::failure(
            "invalid file identifier");
    }
    const auto physical = found->second.file->position();
    if (!physical.succeeded) {
        return physical;
    }
    if (found->second.unreadInput.size() >
            static_cast<size_t>(
                std::numeric_limits<std::int64_t>::max()) ||
        found->second.unreadExtraPhysicalByteOffsets.size() >
            static_cast<size_t>(
                std::numeric_limits<std::int64_t>::max()) ||
        found->second.unreadInput.size() >
            static_cast<size_t>(
                std::numeric_limits<std::int64_t>::max()) -
                found->second.unreadExtraPhysicalByteOffsets.size()) {
        return RuntimeSystemResult<std::int64_t>::failure(
            "buffered input exceeds the supported file position range");
    }
    const auto unreadPhysicalBytes = static_cast<std::int64_t>(
        found->second.unreadInput.size() +
        found->second.unreadExtraPhysicalByteOffsets.size());
    if (physical.value < unreadPhysicalBytes) {
        return RuntimeSystemResult<std::int64_t>::failure(
            "buffered input exceeds the physical file position");
    }
    return RuntimeSystemResult<std::int64_t>::success(
        physical.value - unreadPhysicalBytes);
}

RuntimeSystemStatus RuntimeSystemContext::seekFile(
    int identifier, std::int64_t offset,
    RuntimeFileSeekOrigin origin) {
    std::lock_guard lock(mutex_);
    const auto found = openFiles_.find(identifier);
    if (found == openFiles_.end()) {
        return RuntimeSystemStatus::failure("invalid file identifier");
    }

    std::int64_t physicalOffset = offset;
    if (origin == RuntimeFileSeekOrigin::Current) {
        if (found->second.unreadInput.size() >
            static_cast<size_t>(
                std::numeric_limits<std::int64_t>::max()) ||
            found->second.unreadExtraPhysicalByteOffsets.size() >
                static_cast<size_t>(
                    std::numeric_limits<std::int64_t>::max()) ||
            found->second.unreadInput.size() >
                static_cast<size_t>(
                    std::numeric_limits<std::int64_t>::max()) -
                    found->second.unreadExtraPhysicalByteOffsets.size()) {
            return RuntimeSystemStatus::failure(
                "buffered input is outside the supported seek range");
        }
        const auto unread = static_cast<std::int64_t>(
            found->second.unreadInput.size() +
            found->second.unreadExtraPhysicalByteOffsets.size());
        if (physicalOffset <
            std::numeric_limits<std::int64_t>::min() + unread) {
            return RuntimeSystemStatus::failure(
                "file seek target is outside the supported range");
        }
        physicalOffset -= unread;
    }

    const auto status = found->second.file->seek(
        physicalOffset, origin);
    if (!status.succeeded) {
        return status;
    }
    found->second.unreadInput.clear();
    found->second.unreadExtraPhysicalByteOffsets.clear();
    found->second.pendingReadSize = 0;
    found->second.pendingExtraPhysicalByteOffsets.clear();
    found->second.lastOperation =
        OpenFileEntry::LastOperation::None;
    return RuntimeSystemStatus::success();
}

RuntimeSystemStatus RuntimeSystemContext::closeFile(int identifier) {
    std::lock_guard lock(mutex_);
    const auto found = openFiles_.find(identifier);
    if (found == openFiles_.end()) {
        return RuntimeSystemStatus::failure("invalid file identifier");
    }
    const auto status = found->second.file->close();
    if (!status.succeeded) {
        return status;
    }
    openFiles_.erase(found);
    return RuntimeSystemStatus::success();
}

RuntimeSystemStatus RuntimeSystemContext::closeAllFiles() {
    std::lock_guard lock(mutex_);
    std::string firstError;
    for (auto& [identifier, entry] : openFiles_) {
        (void)identifier;
        const auto status = entry.file->close();
        if (!status.succeeded && firstError.empty()) {
            firstError = status.error;
        }
    }
    openFiles_.clear();
    return firstError.empty()
               ? RuntimeSystemStatus::success()
               : RuntimeSystemStatus::failure(std::move(firstError));
}

RuntimeSystemResult<std::vector<int>>
RuntimeSystemContext::openFileIdentifiers() const {
    std::lock_guard lock(mutex_);
    std::vector<int> result;
    result.reserve(openFiles_.size());
    for (const auto& [identifier, entry] : openFiles_) {
        (void)entry;
        result.push_back(identifier);
    }
    return RuntimeSystemResult<std::vector<int>>::success(
        std::move(result));
}

RuntimeSystemResult<RuntimeOpenFileInfo>
RuntimeSystemContext::openFileInfo(int identifier) const {
    std::lock_guard lock(mutex_);
    const auto found = openFiles_.find(identifier);
    return found == openFiles_.end()
               ? RuntimeSystemResult<RuntimeOpenFileInfo>::failure(
                     "invalid file identifier")
               : RuntimeSystemResult<RuntimeOpenFileInfo>::success(
                     found->second.info);
}

RuntimeSystemResult<std::optional<std::string>>
RuntimeSystemContext::environment(std::string_view name) const {
    const auto permission = require(
        RuntimeSystemCapability::EnvironmentRead,
        "environment access");
    if (!permission.succeeded) {
        return RuntimeSystemResult<std::optional<std::string>>::failure(
            permission.error);
    }
    return hostAdapter_->environment(name);
}

RuntimeSystemResult<RuntimeCalendarTime>
RuntimeSystemContext::localCalendarTime() const {
    const auto permission = require(
        RuntimeSystemCapability::Clock, "clock access");
    if (!permission.succeeded) {
        return RuntimeSystemResult<RuntimeCalendarTime>::failure(
            permission.error);
    }
    return hostAdapter_->localCalendarTime();
}

RuntimeSystemStatus RuntimeSystemContext::sleepFor(
    std::chrono::nanoseconds duration,
    RuntimeExecutionControl* executionControl) const {
    const auto permission = require(
        RuntimeSystemCapability::Sleep, "sleep");
    if (!permission.succeeded) {
        return permission;
    }
    if (duration.count() < 0) {
        return RuntimeSystemStatus::failure(
            "sleep duration cannot be negative");
    }

    constexpr auto quantum = std::chrono::milliseconds(50);
    auto remaining = duration;
    do {
        if (executionControl && !executionControl->checkpoint()) {
            return RuntimeSystemStatus::failure(
                "sleep was stopped by runtime execution control");
        }
        const auto slice = std::min(
            remaining, std::chrono::duration_cast<std::chrono::nanoseconds>(
                           quantum));
        const auto status = hostAdapter_->sleepFor(slice);
        if (!status.succeeded) {
            return status;
        }
        remaining -= slice;
    } while (remaining.count() > 0);
    return RuntimeSystemStatus::success();
}

RuntimeSystemResult<RuntimeProcessOutput>
RuntimeSystemContext::executeProcess(std::string_view command) const {
    const auto permission = require(
        RuntimeSystemCapability::Process, "process execution");
    if (!permission.succeeded) {
        return RuntimeSystemResult<RuntimeProcessOutput>::failure(
            permission.error);
    }
    return hostAdapter_->executeProcess(command);
}

bool RuntimeSystemContext::pauseEnabled() const {
    std::lock_guard lock(mutex_);
    return pauseEnabled_;
}

void RuntimeSystemContext::setPauseEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    pauseEnabled_ = enabled;
}

std::uint64_t RuntimeSystemContext::randomSeed() const {
    std::lock_guard lock(mutex_);
    return randomSeed_;
}

RuntimeSystemResult<RuntimeRandomState>
RuntimeSystemContext::randomState() const {
    const auto permission = require(
        RuntimeSystemCapability::Random, "random-state query");
    if (!permission.succeeded) {
        return RuntimeSystemResult<RuntimeRandomState>::failure(
            permission.error);
    }
    std::lock_guard lock(mutex_);
    std::ostringstream encoded;
    encoded << randomEngine_;
    return RuntimeSystemResult<RuntimeRandomState>::success({
        randomSeed_, encoded.str(), hasSpareNormal_, spareNormal_});
}

RuntimeSystemStatus RuntimeSystemContext::reseedRandom(
    std::uint64_t seed) {
    const auto permission = require(
        RuntimeSystemCapability::Random, "random-state mutation");
    if (!permission.succeeded) {
        return permission;
    }
    std::lock_guard lock(mutex_);
    randomSeed_ = seed;
    randomEngine_.seed(seed);
    hasSpareNormal_ = false;
    spareNormal_ = 0.0;
    return RuntimeSystemStatus::success();
}

RuntimeSystemStatus RuntimeSystemContext::restoreRandomState(
    const RuntimeRandomState& state) {
    const auto permission = require(
        RuntimeSystemCapability::Random, "random-state mutation");
    if (!permission.succeeded) {
        return permission;
    }
    std::istringstream encoded(state.engineState);
    std::mt19937_64 restored;
    if (!(encoded >> restored)) {
        return RuntimeSystemStatus::failure(
            "random state payload is invalid");
    }
    encoded >> std::ws;
    if (!encoded.eof() ||
        (state.hasSpareNormal && !std::isfinite(state.spareNormal))) {
        return RuntimeSystemStatus::failure(
            "random state payload is invalid");
    }
    std::lock_guard lock(mutex_);
    randomSeed_ = state.seed;
    randomEngine_ = std::move(restored);
    hasSpareNormal_ = state.hasSpareNormal;
    spareNormal_ = state.spareNormal;
    return RuntimeSystemStatus::success();
}

RuntimeSystemResult<std::vector<double>>
RuntimeSystemContext::randomUniform(size_t count) {
    const auto permission = require(
        RuntimeSystemCapability::Random, "random generation");
    if (!permission.succeeded) {
        return RuntimeSystemResult<std::vector<double>>::failure(
            permission.error);
    }
    std::vector<double> result(count);
    std::lock_guard lock(mutex_);
    for (double& value : result) {
        value = nextUniform(randomEngine_);
    }
    return RuntimeSystemResult<std::vector<double>>::success(
        std::move(result));
}

RuntimeSystemResult<std::vector<double>>
RuntimeSystemContext::randomNormal(size_t count) {
    const auto permission = require(
        RuntimeSystemCapability::Random, "random generation");
    if (!permission.succeeded) {
        return RuntimeSystemResult<std::vector<double>>::failure(
            permission.error);
    }
    std::vector<double> result;
    result.reserve(count);
    std::lock_guard lock(mutex_);
    if (hasSpareNormal_ && result.size() < count) {
        result.push_back(spareNormal_);
        hasSpareNormal_ = false;
    }
    while (result.size() < count) {
        double first = nextUniform(randomEngine_);
        while (first <= std::numeric_limits<double>::min()) {
            first = nextUniform(randomEngine_);
        }
        const double second = nextUniform(randomEngine_);
        const double magnitude = std::sqrt(-2.0 * std::log(first));
        const double angle = 2.0 * std::numbers::pi * second;
        result.push_back(magnitude * std::cos(angle));
        const double spare = magnitude * std::sin(angle);
        if (result.size() < count) {
            result.push_back(spare);
        } else {
            spareNormal_ = spare;
            hasSpareNormal_ = true;
        }
    }
    return RuntimeSystemResult<std::vector<double>>::success(
        std::move(result));
}

std::shared_ptr<RuntimeSystemContext>
makeIsolatedRuntimeSystemContext() {
    RuntimeSystemContextOptions options;
    options.currentDirectory = std::filesystem::path(".");
    return std::make_shared<RuntimeSystemContext>(std::move(options));
}

std::shared_ptr<RuntimeSystemContext>
makeNativeRuntimeSystemContext(
    std::vector<std::filesystem::path> searchPaths) {
    RuntimeSystemContextOptions options;
    options.capabilities = nativeCapabilities();
    options.hostAdapter = makeNativeRuntimeHostAdapter();
    options.searchPaths = std::move(searchPaths);
    return std::make_shared<RuntimeSystemContext>(std::move(options));
}

} // namespace mparser
