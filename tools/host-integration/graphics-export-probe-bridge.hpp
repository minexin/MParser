#pragma once

#include "mexec/fs/file_store.hpp"
#include <mparser/cpp_api.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <optional>
#include <string>
#include <string_view>

namespace mexec::engine::export_probe {

class HttpHandle {
public:
    explicit HttpHandle(HINTERNET value) : value_(value) {}
    ~HttpHandle() { if (value_) WinHttpCloseHandle(value_); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    HINTERNET get() const { return value_; }
private:
    HINTERNET value_;
};

inline INTERNET_PORT rendererPort() {
    wchar_t text[6]{};
    const auto length = GetEnvironmentVariableW(L"MEXEC_EXPORT_PROBE_PORT", text, 6);
    if (!length || length >= 6) return 0;
    unsigned port = 0;
    for (DWORD i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9') return 0;
        port = port * 10 + static_cast<unsigned>(text[i] - L'0');
    }
    return port <= 65535 ? static_cast<INTERNET_PORT>(port) : 0;
}

inline bool allowedPath(std::string_view path, std::string_view extension) {
    if (!path.ends_with(extension)) return false;
    const auto stem = path.substr(0, path.size() - extension.size());
    // An explicit prefix excludes device names, alternate streams, and any
    // traversal. The candidate does not grant arbitrary session file writes.
    if (!stem.starts_with("export-") || stem.size() <= 7 || stem.size() > 64) return false;
    return stem.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") == std::string_view::npos;
}

inline std::optional<std::string> render(INTERNET_PORT port, bool png,
    std::uint64_t figure, std::string_view snapshot) {
    constexpr DWORD limit = 4 * 1024 * 1024;
    if (snapshot.empty() || snapshot.size() > limit) return std::nullopt;
    const HttpHandle session(WinHttpOpen(L"MExec export test", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get() || !WinHttpSetTimeouts(session.get(), 1000, 1000, 2000, 2000)) return std::nullopt;
    const HttpHandle connection(WinHttpConnect(session.get(), L"127.0.0.1", port, 0));
    if (!connection.get()) return std::nullopt;
    const HttpHandle request(WinHttpOpenRequest(connection.get(), L"POST", png ? L"/png" : L"/pdf",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request.get()) return std::nullopt;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) return std::nullopt;
    const auto headers = L"Content-Type: application/json\r\nX-MParser-Figure: " + std::to_wstring(figure) + L"\r\n";
    std::string body(snapshot);
    const auto length = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
            body.data(), length, length, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) return std::nullopt;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) return std::nullopt;
    DWORD expected = 0;
    size = sizeof(expected);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &expected, &size, WINHTTP_NO_HEADER_INDEX) || !expected || expected > limit) return std::nullopt;
    std::string bytes(expected, '\0');
    for (DWORD offset = 0; offset < expected;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), bytes.data() + offset, expected - offset, &read) || !read) return std::nullopt;
        offset += read;
    }
    if (!bytes.starts_with(png ? "\x89PNG\r\n\x1a\n" : "%PDF-")) return std::nullopt;
    return bytes;
}

inline mparser::sdk::GraphicsExportStatus publish(fs::FileStore& store, std::string_view cwd,
    INTERNET_PORT port, const mparser::sdk::GraphicsExportRequest& request) {
    using Format = mparser::sdk::GraphicsExportFormat;
    using Status = mparser::sdk::GraphicsExportStatus;
    const bool png = request.format == Format::Png;
    if (!png && request.format != Format::Pdf) return Status::UnsupportedFormat;
    if (!cwd.empty() || !allowedPath(request.destination, png ? ".png" : ".pdf")) return Status::DestinationRejected;
    const DWORD attributes = GetFileAttributesW(store.root().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return Status::DestinationRejected;
    if (request.snapshotVersion != 4) return Status::Failed;
    const auto bytes = render(port, png, request.figureIdentity, request.sceneJson);
    if (!bytes) return Status::Failed;
    std::string error;
    return store.writeText(std::string(request.destination), *bytes, &error) ? Status::Written : Status::Failed;
}

} // namespace mexec::engine::export_probe
