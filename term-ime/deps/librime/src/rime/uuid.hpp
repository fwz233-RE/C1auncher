//
// Minimal boost::uuids replacement for term-ime.
// On Linux, reads a random UUID v4 from /proc/sys/kernel/random/uuid (same
// format boost::uuids::to_string produces). Exposes boost::uuids::to_string
// and a random_generator-compatible callable so the call site compiles.
//
#ifndef RIME_UUID_HPP_
#define RIME_UUID_HPP_

#include <fstream>
#include <string>

namespace rime {
inline std::string random_uuid_string() {
  std::ifstream f("/proc/sys/kernel/random/uuid");
  std::string s;
  if (f >> s && s.size() == 36) return s;
  // Fallback (shouldn't happen on Linux): a deterministic-looking id.
  return "00000000-0000-4000-8000-000000000000";
}
}  // namespace rime

namespace boost {
namespace uuids {
struct random_generator {
  std::string operator()() const { return rime::random_uuid_string(); }
};
inline std::string to_string(const std::string& uuid) { return uuid; }
}  // namespace uuids
}  // namespace boost

#endif  // RIME_UUID_HPP_
