//
// Minimal boost::signals2 replacement for term-ime.
//
// Provides signal<Signature> and connection with the subset of the signals2 API
// that librime uses:
//   - connect(slot) -> connection
//   - connection::disconnect()  (removes that slot)
//   - connection default-constructible / copyable (held as class members)
//   - operator()(args...) to emit to all connected slots
// Not thread-safe (librime drives everything on one thread).
//
#ifndef RIME_SIGNAL_HPP_
#define RIME_SIGNAL_HPP_

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace rime {

// Shared slot control block: a slot is "alive" until disconnect() flips this.
// Defined outside connection so signal<> can name it without a nested type
// (which complicates ADL with std headers included in the same TU).
struct slot_control {
  bool alive = true;
};

// A connection is a weak handle to one slot's control block. disconnect()
// marks the slot dead (the owning signal skips dead slots on emit). Default-
// constructible and copyable so it can be stored as a member and reassigned.
class connection {
 public:
  connection() = default;
  explicit connection(std::shared_ptr<slot_control> ctrl) : ctrl_(std::move(ctrl)) {}

  void disconnect() {
    if (auto c = ctrl_.lock()) c->alive = false;
  }
  bool connected() const {
    auto c = ctrl_.lock();
    return c && c->alive;
  }

 private:
  std::weak_ptr<slot_control> ctrl_;
};

template <typename Signature>
class signal;

template <typename... Args>
class signal<void(Args...)> {
 public:
  using slot_type = std::function<void(Args...)>;

  connection connect(slot_type slot) {
    auto ctrl = std::make_shared<slot_control>();
    slots_.push_back({ctrl, std::move(slot)});
    return connection(ctrl);
  }

  void operator()(Args... args) const {
    // Slots may disconnect() during emission, which only flips `alive`; the
    // vector itself is not mutated mid-iteration.
    for (auto& e : slots_) {
      if (e.ctrl->alive) e.fn(args...);
    }
  }

  void disconnect_all_slots() { slots_.clear(); }

 private:
  struct Entry {
    std::shared_ptr<slot_control> ctrl;
    slot_type fn;
  };
  mutable std::vector<Entry> slots_;
};

}  // namespace rime

#endif  // RIME_SIGNAL_HPP_
