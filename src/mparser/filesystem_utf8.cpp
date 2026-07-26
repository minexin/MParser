#include "mparser/filesystem_utf8.h"

#include <stdexcept>
#include <string>

namespace mparser {

bool isValidUtf8(std::string_view value) noexcept {
    size_t index = 0;
    while (index < value.size()) {
        const auto first =
            static_cast<unsigned char>(value[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }

        size_t length = 0;
        char32_t codePoint = 0;
        char32_t minimum = 0;
        if ((first & 0xe0) == 0xc0) {
            length = 2;
            codePoint = first & 0x1f;
            minimum = 0x80;
        } else if ((first & 0xf0) == 0xe0) {
            length = 3;
            codePoint = first & 0x0f;
            minimum = 0x800;
        } else if ((first & 0xf8) == 0xf0) {
            length = 4;
            codePoint = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (length > value.size() - index) {
            return false;
        }
        for (size_t offset = 1; offset < length; ++offset) {
            const auto next =
                static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            codePoint =
                (codePoint << 6) | (next & 0x3f);
        }
        if (codePoint < minimum || codePoint > 0x10ffff ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
            return false;
        }
        index += length;
    }
    return true;
}

std::filesystem::path pathFromUtf8(std::string_view value) {
    if (!isValidUtf8(value)) {
        throw std::invalid_argument(
            "filesystem path is not valid UTF-8");
    }
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char byte : value) {
        encoded.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path(encoded);
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size());
}

} // namespace mparser
