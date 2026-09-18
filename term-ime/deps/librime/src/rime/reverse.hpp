//
// Minimal boost::range::adaptors::reverse replacement for C++17.
// rime::reverse(c) yields an object usable in range-for to iterate c in
// reverse order: `for (auto& x : rime::reverse(vec))`.
//
#ifndef RIME_REVERSE_HPP_
#define RIME_REVERSE_HPP_

#include <iterator>
#include <utility>

namespace rime {

template <typename T>
class reverse_view {
 public:
  explicit reverse_view(T& c) : c_(c) {}
  auto begin() { return std::rbegin(c_); }
  auto end() { return std::rend(c_); }
  auto begin() const { return std::rbegin(c_); }
  auto end() const { return std::rend(c_); }

 private:
  T& c_;
};

template <typename T>
reverse_view<T> reverse(T& c) {
  return reverse_view<T>(c);
}

}  // namespace rime

#endif  // RIME_REVERSE_HPP_
