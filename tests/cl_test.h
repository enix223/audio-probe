#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

std::vector<TestCase>& Registry();
void Fail(const char* file, int line, const std::string& message);

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    Registry().push_back(TestCase{name, std::move(fn)});
  }
};

template <typename A, typename B>
void ExpectEq(const char* file, int line, const char* expr, const A& a, const B& b) {
  if (a == b) return;
  std::ostringstream os;
  os << expr << " produced different values";
  Fail(file, line, os.str());
}

}  // namespace testing

#define TEST(name)                                              \
  static void test_##name();                                    \
  static testing::Registrar reg_##name(#name, test_##name);      \
  static void test_##name()

#define EXPECT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) testing::Fail(__FILE__, __LINE__, "EXPECT_TRUE: " #cond); \
  } while (0)

#define EXPECT_FALSE(cond)                                                \
  do {                                                                    \
    if ((cond)) testing::Fail(__FILE__, __LINE__, "EXPECT_FALSE: " #cond); \
  } while (0)

#define EXPECT_EQ(a, b)                                                    \
  do {                                                                     \
    const auto _a = (a);                                                   \
    const auto _b = (b);                                                   \
    if (!(_a == _b)) {                                                     \
      std::ostringstream _os;                                              \
      _os << "EXPECT_EQ(" #a ", " #b ") -> " << _a << " vs " << _b;        \
      testing::Fail(__FILE__, __LINE__, _os.str());                        \
    }                                                                      \
  } while (0)

#define EXPECT_NEAR(a, b, tol)                                             \
  do {                                                                     \
    const double _a = static_cast<double>(a);                              \
    const double _b = static_cast<double>(b);                              \
    const double _t = static_cast<double>(tol);                            \
    if (std::fabs(_a - _b) > _t) {                                         \
      std::ostringstream _os;                                              \
      _os << "EXPECT_NEAR(" #a ", " #b ", " #tol ") -> " << _a << " vs "   \
          << _b << " (tol " << _t << ")";                                  \
      testing::Fail(__FILE__, __LINE__, _os.str());                        \
    }                                                                      \
  } while (0)

#define ASSERT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) {                                                       \
      testing::Fail(__FILE__, __LINE__, "ASSERT_TRUE: " #cond);          \
      return;                                                            \
    }                                                                    \
  } while (0)
