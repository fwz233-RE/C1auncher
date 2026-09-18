#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace utf8 {

// Get number of bytes in a UTF-8 character from first byte
inline int char_len(uint8_t first_byte) {
    if ((first_byte & 0x80) == 0x00)
        return 1;
    if ((first_byte & 0xE0) == 0xC0)
        return 2;
    if ((first_byte & 0xF0) == 0xE0)
        return 3;
    if ((first_byte & 0xF8) == 0xF0)
        return 4;
    return 1;
}

// Decode UTF-8 sequence to UTF-32
char32_t decode(const uint8_t* data, size_t len, size_t& pos);

// Encode UTF-32 to UTF-8
std::string encode(char32_t ch);

// Get display width (1 for ASCII, 2 for CJK, etc.)
// Uses utf8proc for proper Unicode width calculation
int width(char32_t ch);

// Get display width of a UTF-8 string
int string_width(const std::string& str);

}  // namespace utf8
