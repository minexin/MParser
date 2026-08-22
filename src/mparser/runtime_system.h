#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {

class RuntimeExecutionControl;

enum class RuntimeSystemCapability : std::uint32_t {
    None = 0,
    CurrentDirectory = 1U << 0U,
    SearchPaths = 1U << 1U,
    EnvironmentRead = 1U << 2U,
    FileSystemRead = 1U << 3U,
    Process = 1U << 4U,
    Clock = 1U << 5U,
    Sleep = 1U << 6U,
    Random = 1U << 7U,
    DynamicEvaluation = 1U << 8U,
    FileSystemWrite = 1U << 9U,
};

RuntimeSystemCapability operator|(RuntimeSystemCapability left,
                                  RuntimeSystemCapability right);
RuntimeSystemCapability operator&(RuntimeSystemCapability left,
                                  RuntimeSystemCapability right);
bool hasRuntimeSystemCapability(RuntimeSystemCapability value,
                                RuntimeSystemCapability expected);

template <typename T>
struct RuntimeSystemResult {
    bool succeeded = false;
    T value{};
    std::string error;

    static RuntimeSystemResult success(T result) {
        RuntimeSystemResult value;
        value.succeeded = true;
        value.value = std::move(result);
        return value;
    }

    static RuntimeSystemResult failure(std::string message) {
        RuntimeSystemResult value;
        value.error = std::move(message);
        return value;
    }
};

struct RuntimeSystemStatus {
    bool succeeded = false;
    std::string error;

    static RuntimeSystemStatus success();
    static RuntimeSystemStatus failure(std::string message);
};

struct RuntimeCalendarTime {
    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    double second = 0.0;
};

struct RuntimeDirectoryEntry {
    std::string name;
    std::string folder;
    std::string date;
    std::uintmax_t bytes = 0;
    bool directory = false;
};

struct RuntimeProcessOutput {
    int status = 0;
    std::string output;
};

struct RuntimeFileOpenOptions {
    bool readable = false;
    bool writable = false;
    bool append = false;
    bool truncate = false;
    bool binary = true;
    std::string permission;
    std::string machineFormat;
    std::string encoding = "UTF-8";
};

enum class RuntimeFileSeekOrigin {
    Beginning,
    Current,
    End,
};

struct RuntimeFileReadBuffer {
    std::string text;
    // A point N means the first N logical bytes consumed one extra physical
    // byte, currently used for Windows CRLF text translation.
    std::vector<size_t> extraPhysicalByteOffsets;
};

struct RuntimeFileError {
    std::string message;
    int number = 0;
};

class RuntimeHostFile {
public:
    virtual ~RuntimeHostFile() = default;

    virtual RuntimeSystemResult<size_t>
    write(std::string_view text) = 0;
    virtual RuntimeSystemResult<RuntimeFileReadBuffer>
    readRemaining(size_t maximumBytes) = 0;
    virtual RuntimeSystemResult<std::int64_t> position() = 0;
    virtual RuntimeSystemStatus seek(
        std::int64_t offset, RuntimeFileSeekOrigin origin) = 0;
    virtual RuntimeSystemStatus close() = 0;
};

struct RuntimeOpenFileInfo {
    int identifier = -1;
    std::filesystem::path path;
    std::filesystem::path resolvedPath;
    RuntimeFileOpenOptions options;
};

struct RuntimeRandomState {
    std::uint64_t seed = 5489U;
    std::string engineState;
    bool hasSpareNormal = false;
    double spareNormal = 0.0;
};

class RuntimeHostAdapter {
public:
    virtual ~RuntimeHostAdapter() = default;

    virtual RuntimeSystemResult<std::filesystem::path>
    processCurrentDirectory() const = 0;
    virtual RuntimeSystemResult<std::filesystem::path>
    temporaryDirectory() const = 0;
    virtual RuntimeSystemResult<std::filesystem::path>
    normalizeDirectory(const std::filesystem::path& base,
                       const std::filesystem::path& candidate) const = 0;
    virtual RuntimeSystemResult<std::vector<RuntimeDirectoryEntry>>
    listDirectory(const std::filesystem::path& path) const = 0;
    virtual RuntimeSystemResult<bool>
    regularFileExists(const std::filesystem::path& path) const = 0;
    virtual RuntimeSystemResult<bool>
    directoryExists(const std::filesystem::path& path) const = 0;
    virtual RuntimeSystemResult<std::shared_ptr<RuntimeHostFile>>
    openFile(const std::filesystem::path& path,
             const RuntimeFileOpenOptions& options) const = 0;
    virtual RuntimeSystemResult<std::optional<std::string>>
    environment(std::string_view name) const = 0;
    virtual RuntimeSystemResult<RuntimeCalendarTime>
    localCalendarTime() const = 0;
    virtual RuntimeSystemStatus sleepFor(
        std::chrono::nanoseconds duration) const = 0;
    virtual RuntimeSystemResult<RuntimeProcessOutput>
    executeProcess(std::string_view command) const = 0;
};

std::shared_ptr<RuntimeHostAdapter> makeNativeRuntimeHostAdapter();

struct RuntimeSystemContextOptions {
    RuntimeSystemCapability capabilities =
        RuntimeSystemCapability::None;
    std::shared_ptr<RuntimeHostAdapter> hostAdapter;
    std::vector<std::filesystem::path> searchPaths;
    std::optional<std::filesystem::path> currentDirectory;
    std::uint64_t randomSeed = 5489U;
    size_t maximumOpenFiles = 256;
    size_t maximumFileReadBytes = 16U * 1024U * 1024U;
};

class RuntimeSystemContext {
public:
    explicit RuntimeSystemContext(
        RuntimeSystemContextOptions options = {});
    ~RuntimeSystemContext();

    RuntimeSystemCapability capabilities() const noexcept;
    bool hasCapability(RuntimeSystemCapability capability) const noexcept;

    RuntimeSystemResult<std::filesystem::path>
    currentDirectory() const;
    RuntimeSystemStatus changeCurrentDirectory(
        const std::filesystem::path& path);
    RuntimeSystemResult<std::filesystem::path>
    temporaryDirectory() const;

    RuntimeSystemResult<std::vector<std::filesystem::path>>
    searchPaths() const;
    RuntimeSystemStatus setSearchPaths(
        std::vector<std::filesystem::path> paths);
    RuntimeSystemStatus addSearchPaths(
        const std::vector<std::filesystem::path>& paths,
        bool prepend);
    RuntimeSystemStatus removeSearchPaths(
        const std::vector<std::filesystem::path>& paths);

    RuntimeSystemResult<std::vector<RuntimeDirectoryEntry>>
    listDirectory(const std::filesystem::path& path) const;
    RuntimeSystemResult<bool> regularFileExists(
        const std::filesystem::path& path) const;
    RuntimeSystemResult<bool> directoryExists(
        const std::filesystem::path& path) const;
    RuntimeSystemResult<int> openFile(
        const std::filesystem::path& path,
        const RuntimeFileOpenOptions& options);
    RuntimeSystemResult<size_t> writeFile(
        int identifier, std::string_view text);
    RuntimeSystemResult<size_t> writeFileBlocks(
        int identifier, std::string_view bytes,
        size_t blockBytes, size_t skipBytes);
    RuntimeSystemResult<std::string> readFileRemaining(
        int identifier);
    RuntimeSystemStatus restoreUnreadFileData(
        int identifier, std::string text);
    RuntimeSystemResult<std::int64_t> filePosition(
        int identifier);
    RuntimeSystemStatus seekFile(
        int identifier, std::int64_t offset,
        RuntimeFileSeekOrigin origin);
    RuntimeSystemResult<bool> fileEndOfFile(int identifier) const;
    RuntimeSystemResult<RuntimeFileError> fileError(
        int identifier, bool clear);
    RuntimeSystemStatus closeFile(int identifier);
    RuntimeSystemStatus closeAllFiles();
    RuntimeSystemResult<std::vector<int>>
    openFileIdentifiers() const;
    RuntimeSystemResult<RuntimeOpenFileInfo>
    openFileInfo(int identifier) const;
    RuntimeSystemResult<std::optional<std::string>>
    environment(std::string_view name) const;
    RuntimeSystemResult<RuntimeCalendarTime>
    localCalendarTime() const;
    RuntimeSystemStatus sleepFor(
        std::chrono::nanoseconds duration,
        RuntimeExecutionControl* executionControl) const;
    bool pauseEnabled() const;
    void setPauseEnabled(bool enabled);
    RuntimeSystemResult<RuntimeProcessOutput>
    executeProcess(std::string_view command) const;

    std::uint64_t randomSeed() const;
    RuntimeSystemResult<RuntimeRandomState> randomState() const;
    RuntimeSystemStatus reseedRandom(std::uint64_t seed);
    RuntimeSystemStatus restoreRandomState(
        const RuntimeRandomState& state);
    RuntimeSystemResult<std::vector<double>> randomUniform(size_t count);
    RuntimeSystemResult<std::vector<double>> randomNormal(size_t count);

private:
    RuntimeSystemStatus require(
        RuntimeSystemCapability capability,
        std::string_view operation) const;
    RuntimeSystemResult<std::filesystem::path>
    normalizeDirectory(const std::filesystem::path& path) const;

    RuntimeSystemCapability capabilities_ =
        RuntimeSystemCapability::None;
    std::shared_ptr<RuntimeHostAdapter> hostAdapter_;
    mutable std::mutex mutex_;
    std::filesystem::path currentDirectory_;
    std::vector<std::filesystem::path> searchPaths_;
    struct OpenFileEntry {
        enum class LastOperation {
            None,
            Read,
            Write,
        };

        RuntimeOpenFileInfo info;
        std::shared_ptr<RuntimeHostFile> file;
        std::string unreadInput;
        std::vector<size_t> unreadExtraPhysicalByteOffsets;
        size_t pendingReadSize = 0;
        std::vector<size_t> pendingExtraPhysicalByteOffsets;
        bool pendingReadReachedEnd = false;
        bool endOfFileReached = false;
        RuntimeFileError error;
        LastOperation lastOperation = LastOperation::None;
    };
    std::map<int, OpenFileEntry> openFiles_;
    int nextFileIdentifier_ = 3;
    size_t maximumOpenFiles_ = 256;
    size_t maximumFileReadBytes_ = 16U * 1024U * 1024U;
    std::uint64_t randomSeed_ = 5489U;
    std::mt19937_64 randomEngine_;
    bool pauseEnabled_ = true;
    bool hasSpareNormal_ = false;
    double spareNormal_ = 0.0;
};

std::shared_ptr<RuntimeSystemContext>
makeIsolatedRuntimeSystemContext();

std::shared_ptr<RuntimeSystemContext>
makeNativeRuntimeSystemContext(
    std::vector<std::filesystem::path> searchPaths = {});

} // namespace mparser
