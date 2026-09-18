//
// Minimal boost::crc_32_type replacement for term-ime.
// Standard CRC-32 (zlib polynomial 0xEDB88320 reflected). API: process_bytes,
// checksum — matches boost::crc_32_type usage in utilities.{h,cc}.
//
#ifndef RIME_CRC32_HPP_
#define RIME_CRC32_HPP_

#include <array>
#include <cstdint>
#include <cstddef>

namespace rime {

class crc_32_type {
 public:
  explicit crc_32_type(uint32_t /*initial_remainder*/ = 0) : crc_(0xFFFFFFFFu) {}
  void process_bytes(const void* data, std::size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i)
      crc_ = table()[(crc_ ^ p[i]) & 0xFF] ^ (crc_ >> 8);
  }
  uint32_t checksum() const { return ~crc_; }

 private:
  // Function-local static: initialized once on first use (thread-safe via
  // C++11 magic statics), no separate definition needed.
  static const std::array<uint32_t, 256>& table() {
    static const std::array<uint32_t, 256> t = [] {
      std::array<uint32_t, 256> a{};
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
          c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        a[i] = c;
      }
      return a;
    }();
    return t;
  }
  uint32_t crc_;
};

}  // namespace rime

namespace boost { using rime::crc_32_type; }

#endif  // RIME_CRC32_HPP_
