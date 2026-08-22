#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mparser {

bool isValidUtf8(std::string_view value) noexcept;
std::filesystem::path pathFromUtf8(std::string_view value);
std::string pathToUtf8(const std::filesystem::path& path);
std::string pathToNativeUtf8(const std::filesystem::path& path);

} // namespace mparser
