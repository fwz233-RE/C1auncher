#include "utf8.hpp"
#include <utf8proc.h>

namespace utf8 {

char32_t decode(const uint8_t* data, size_t len, size_t& pos) {
    if (pos >= len)
        return U'\0';

    // Use utf8proc for proper decoding
    int32_t codepoint = 0;
    utf8proc_ssize_t bytes_read = utf8proc_iterate(data + pos, len - pos, &codepoint);

    if (bytes_read < 0) {
        // Invalid UTF-8, skip one byte
        pos++;
        return U'�';  // Replacement character
    }

    pos += bytes_read;
    return static_cast<char32_t>(codepoint);
}

std::string encode(char32_t ch) {
    if (ch == 0 || ch > 0x10FFFF) {
        return " ";
    }

    uint8_t buffer[4];
    utf8proc_ssize_t len = utf8proc_encode_char(static_cast<int32_t>(ch), buffer);

    if (len <= 0) {
        return " ";
    }

    return std::string(reinterpret_cast<char*>(buffer), len);
}

int width(char32_t ch) {
    // Use utf8proc for proper character width
    // utf8proc_charwidth returns 0, 1, or 2 based on Unicode properties
    return utf8proc_charwidth(static_cast<int32_t>(ch));
}

int string_width(const std::string& str) {
    int total_width = 0;
    size_t pos = 0;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());

    while (pos < str.size()) {
        int32_t codepoint = 0;
        utf8proc_ssize_t bytes_read = utf8proc_iterate(data + pos, str.size() - pos, &codepoint);

        if (bytes_read < 0) {
            pos++;
            continue;
        }

        total_width += utf8proc_charwidth(codepoint);
        pos += bytes_read;
    }

    return total_width;
}

}  // namespace utf8
