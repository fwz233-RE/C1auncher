//
// Minimal boost::algorithm/string replacement for term-ime.
//
// Implements the subset of boost::algorithm/string that librime uses, exposed
// under the `boost::` namespace so call sites compile unchanged (only the
// #include changes). Covers: starts_with, ends_with, contains, to_lower,
// trim, trim_left, trim_right, trim_left_if, trim_right_if, trim_left_copy_if,
// trim_right_copy_if, split, join, is_any_of, is_from_range, erase_last,
// erase_last_copy, erase_first_copy, find_last (returns a range proxy with
// begin()/end()/empty()).
//
#ifndef RIME_STRING_UTILS_HPP_
#define RIME_STRING_UTILS_HPP_

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace boost {

// Predicates -------------------------------------------------------------
template <class CharT>
struct is_any_of_t {
  std::basic_string<CharT> set;
  bool operator()(CharT c) const {
    return set.find(c) != std::basic_string<CharT>::npos;
  }
};
template <class CharT>
is_any_of_t<CharT> is_any_of(const CharT* s) {
  return is_any_of_t<CharT>{std::basic_string<CharT>(s)};
}
template <class CharT>
is_any_of_t<CharT> is_any_of(const std::basic_string<CharT>& s) {
  return is_any_of_t<CharT>{s};
}
// is_from_range(a, b): predicate true for any char in [a,b].
template <class CharT>
struct is_from_range_t {
  CharT lo, hi;
  bool operator()(CharT c) const { return c >= lo && c <= hi; }
};
template <class CharT>
is_from_range_t<CharT> is_from_range(CharT a, CharT b) {
  return is_from_range_t<CharT>{a, b};
}

// starts_with / ends_with / contains ------------------------------------
inline bool starts_with(const std::string& s, const std::string& sub) {
  return s.size() >= sub.size() &&
         s.compare(0, sub.size(), sub) == 0;
}
inline bool starts_with(const std::string& s, const char* sub) {
  return starts_with(s, std::string(sub));
}
inline bool ends_with(const std::string& s, const std::string& sub) {
  return s.size() >= sub.size() &&
         s.compare(s.size() - sub.size(), sub.size(), sub) == 0;
}
inline bool ends_with(const std::string& s, const char* sub) {
  return ends_with(s, std::string(sub));
}
inline bool contains(const std::string& s, const std::string& sub) {
  return s.find(sub) != std::string::npos;
}
inline bool contains(const std::string& s, const char* sub) {
  return s.find(sub) != std::string::npos;
}

// to_lower / to_upper (in-place) ----------------------------------------
inline void to_lower(std::string& s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
}
inline void to_upper(std::string& s) {
  for (char& c : s) c = (char)std::toupper((unsigned char)c);
}

// trim (default: whitespace) -------------------------------------------
namespace detail {
inline bool is_space(char c) { return std::isspace((unsigned char)c) != 0; }
}  // namespace detail

inline void trim_left(std::string& s) {
  s.erase(s.begin(),
          std::find_if_not(s.begin(), s.end(), detail::is_space));
}
inline void trim_right(std::string& s) {
  s.erase(std::find_if_not(s.rbegin(), s.rend(), detail::is_space).base(),
          s.end());
}
inline void trim(std::string& s) {
  trim_left(s);
  trim_right(s);
}
inline void trim_left_if(std::string& s, std::function<bool(char)> pred) {
  s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), pred));
}
inline void trim_right_if(std::string& s, std::function<bool(char)> pred) {
  s.erase(std::find_if_not(s.rbegin(), s.rend(), pred).base(), s.end());
}
inline void trim_if(std::string& s, std::function<bool(char)> pred) {
  trim_left_if(s, pred);
  trim_right_if(s, pred);
}
inline std::string trim_left_copy_if(const std::string& s,
                                     std::function<bool(char)> pred) {
  auto it = std::find_if_not(s.begin(), s.end(), pred);
  return std::string(it, s.end());
}
inline std::string trim_right_copy_if(const std::string& s,
                                      std::function<bool(char)> pred) {
  auto it = std::find_if_not(s.rbegin(), s.rend(), pred).base();
  return std::string(s.begin(), it);
}
inline std::string trim_left_copy(const std::string& s) {
  return trim_left_copy_if(s, detail::is_space);
}
inline std::string trim_right_copy(const std::string& s) {
  return trim_right_copy_if(s, detail::is_space);
}
inline std::string trim_copy(const std::string& s) {
  return trim_left_copy(trim_right_copy(s));
}

// split ------------------------------------------------------------------
// boost::split(out, in, pred) splits at every char where pred(c); empty tokens
// are kept (boost default token_compress_off).
template <class Pred>
void split(std::vector<std::string>& out, const std::string& in, Pred pred) {
  out.clear();
  std::string cur;
  for (char c : in) {
    if (pred(c)) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
}

// join -------------------------------------------------------------------
template <class Range>
std::string join(const Range& v, const char* sep) {
  std::string r;
  bool first = true;
  for (const auto& e : v) {
    if (!first) r += sep;
    r += e;
    first = false;
  }
  return r;
}
template <class Range>
std::string join(const Range& v, const std::string& sep) {
  return join(v, sep.c_str());
}

// erase_first / erase_last ---------------------------------------------
inline void erase_first(std::string& s, const std::string& sub) {
  auto pos = s.find(sub);
  if (pos != std::string::npos) s.erase(pos, sub.size());
}
inline void erase_last(std::string& s, const std::string& sub) {
  auto pos = s.rfind(sub);
  if (pos != std::string::npos) s.erase(pos, sub.size());
}
inline std::string erase_first_copy(std::string s, const std::string& sub) {
  erase_first(s, sub);
  return s;
}
inline std::string erase_last_copy(std::string s, const std::string& sub) {
  erase_last(s, sub);
  return s;
}

// find_last — returns a proxy range over the last match (begin/end/empty).
struct find_result {
  std::string::const_iterator b, e;
  std::string::const_iterator begin() const { return b; }
  std::string::const_iterator end() const { return e; }
  bool empty() const { return b == e; }
  operator bool() const { return !empty(); }
};
inline find_result find_last(const std::string& s, const std::string& sub) {
  auto pos = s.rfind(sub);
  if (pos == std::string::npos) return {s.end(), s.end()};
  return {s.begin() + pos, s.begin() + pos + sub.size()};
}

}  // namespace boost

// Re-export the string algorithms under boost::algorithm:: (some librime call
// sites use the fully-qualified boost::algorithm::xxx form).
namespace boost {
namespace algorithm {
using boost::starts_with;
using boost::ends_with;
using boost::contains;
using boost::to_lower;
using boost::to_upper;
using boost::trim_left;
using boost::trim_right;
using boost::trim;
using boost::trim_left_if;
using boost::trim_right_if;
using boost::trim_if;
using boost::trim_left_copy_if;
using boost::trim_right_copy_if;
using boost::trim_left_copy;
using boost::trim_right_copy;
using boost::trim_copy;
using boost::is_any_of;
using boost::is_from_range;
using boost::erase_first;
using boost::erase_last;
using boost::erase_first_copy;
using boost::erase_last_copy;
using boost::find_last;
using boost::join;
using boost::split;
}  // namespace algorithm
}  // namespace boost

#endif  // RIME_STRING_UTILS_HPP_
