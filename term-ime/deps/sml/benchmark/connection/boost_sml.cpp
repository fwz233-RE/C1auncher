struct connect{};
struct established{};
struct ping{};
struct disconnect{};
struct timeout{};

#if defined(TEST_PERF) or defined(TEST_GBENCH)
  static void clobber() { asm volatile("" : : : "memory"); }
  const auto establish = []{ clobber(); };
  const auto close = []{ clobber(); };
  const auto is_valid = [](auto const&) { clobber(); return true; };
  const auto reset_timeout = [] { clobber(); };
#else
  #include <cstdio>
  constexpr auto establish = []{ std::puts("establish"); };
  constexpr auto close = []{ std::puts("close"); };
  constexpr auto is_valid = [](auto const&) { return true; };
  constexpr auto reset_timeout = []{ std::puts("reset_timeout"); };
#endif

#include <boost/sml.hpp>

namespace sml = boost::sml;

struct connection {
  auto operator()() const {
    using namespace sml;
    return make_transition_table(
      * "Disconnected"_s + event<connect> / establish                = "Connecting"_s,
        "Connecting"_s   + event<established>                        = "Connected"_s,
        "Connected"_s    + event<ping> [ is_valid ] / reset_timeout,
        "Connected"_s    + event<timeout> / establish                = "Connecting"_s,
        "Connected"_s    + event<disconnect> / close                 = "Disconnected"_s
    );
  }
};

using Connection = sml::sm<connection, sml::dispatch<sml::back::policies::fold_expr>>;

#if defined(TEST_ASM)
  int main() {
    Connection connection{};
    connection.process_event(connect{});
    connection.process_event(established{});
    connection.process_event(ping{});
    connection.process_event(disconnect{});
    connection.process_event(connect{});
    connection.process_event(established{});
    connection.process_event(ping{});
  }
#elif defined(TEST_ASM_GENERIC)
  #include <cstdlib>

  int main() {
    Connection connection{};

    while(true) {
      switch(rand() % 5) {
        default: break;
        case 0: connection.process_event(connect{}); break;
        case 1: connection.process_event(established{}); break;
        case 2: connection.process_event(ping{}); break;
        case 3: connection.process_event(disconnect{}); break;
        case 4: connection.process_event(timeout{}); break;
      }
    }
  }
#elif defined(TEST_PERF)
  int main() {
    Connection connection{};

    for (auto i = 0; i < 100'000'000; ++i) {
      connection.process_event(connect{});
      connection.process_event(established{});
      connection.process_event(ping{});
      connection.process_event(disconnect{});
    }
  }
#elif defined(TEST_GBENCH)
  #include <benchmark/benchmark.h>

  static void BM_boost_sml(benchmark::State& state) {
    constexpr auto size = 1'000'000;

    int dispatch[size]{};
    for (auto i = 0; i < size; ++i) {
      dispatch[i] = rand() % 4;
    }

    Connection connection;

    auto i = 0;
    for (auto _ : state) {
      switch(dispatch[i++]) {
        default: break;
        case 0: connection.process_event(connect{}); break;
        case 1: connection.process_event(established{}); break;
        case 2: connection.process_event(ping{}); break;
        case 3: connection.process_event(disconnect{}); break;
        case 4: connection.process_event(timeout{}); break;
      }
      if (i++ >= size) i = {};
    }
  }

  BENCHMARK(BM_boost_sml);
  BENCHMARK_MAIN();
#endif
