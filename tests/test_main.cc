#include "cl_test.h"

#include <cstdio>
#include <exception>

namespace testing {

int g_failures = 0;

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

void Fail(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, message.c_str());
}

}  // namespace testing

int main() {
  for (const auto& tc : testing::Registry()) {
    std::fprintf(stderr, "[ RUN ] %s\n", tc.name.c_str());
    try {
      tc.fn();
    } catch (const std::exception& e) {
      ++testing::g_failures;
      std::fprintf(stderr, "FAIL %s: exception: %s\n", tc.name.c_str(), e.what());
    } catch (...) {
      ++testing::g_failures;
      std::fprintf(stderr, "FAIL %s: unknown exception\n", tc.name.c_str());
    }
  }
  if (testing::g_failures > 0) {
    std::fprintf(stderr, "%d failure(s) across %zu tests\n", testing::g_failures,
                 testing::Registry().size());
    return 1;
  }
  std::fprintf(stderr, "all %zu tests passed\n", testing::Registry().size());
  return 0;
}
