//
// Minimal boost::scope_exit replacement for term-ime (C++17).
//
// Usage: `auto _se = rime::make_scope_exit([&]{ ... });` — the lambda runs when
// _se goes out of scope. Replaces BOOST_SCOPE_EXIT((&x)) { ... } SCOPE_EXIT_END.
//
#ifndef RIME_SCOPE_EXIT_HPP_
#define RIME_SCOPE_EXIT_HPP_

#include <functional>
#include <utility>

namespace rime {

class scope_exit {
 public:
  explicit scope_exit(std::function<void()> fn) : fn_(std::move(fn)) {}
  ~scope_exit() {
    if (fn_) fn_();
  }
  scope_exit(const scope_exit&) = delete;
  scope_exit& operator=(const scope_exit&) = delete;
  void release() { fn_ = nullptr; }

 private:
  std::function<void()> fn_;
};

inline scope_exit make_scope_exit(std::function<void()> fn) {
  return scope_exit(std::move(fn));
}

}  // namespace rime

#endif  // RIME_SCOPE_EXIT_HPP_
