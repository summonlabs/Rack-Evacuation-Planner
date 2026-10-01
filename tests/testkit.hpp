// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic test framework for the Rack Evacuation Planner suite.
//
// A test file declares its tests at file scope and never writes main():
//
//   #include "testkit.hpp"
//
//   REP_TEST(Suite, name) {
//     REP_CHECK_EQ(2 + 2, 4);
//   }
//
//   REP_TEST_MAIN()
//
// Registration is static, so the registry keeps the tests in declaration
// order.  Nothing here uses threads, clocks, signals, or timeouts: a test runs
// to natural completion, a recorded failure is reported and the run continues
// with the next test, and a test that throws is reported as failed rather than
// taking the process down.

#ifndef REP_TESTS_TESTKIT_HPP
#define REP_TESTS_TESTKIT_HPP

#include <cstddef>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace reptest {

// Renders one value for a diagnostic.  A type with an operator<< is streamed;
// any other type - every equality-comparable type without one - is reported as
// "<unprintable>" instead of failing to compile.
template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (requires(std::ostream& stream, const T& item) { stream << item; }) {
    std::ostringstream text;
    text << value;
    return text.str();
  } else {
    return "<unprintable>";
  }
}

namespace detail {

using TestBody = void (*)();

// Routes a check condition through a function call.  A test that checks a
// constant expression - REP_CHECK(true), REP_CHECK_EQ(2 + 2, 4) - must not
// trip MSVC's "conditional expression is constant" warning under /W4 /WX.
[[nodiscard]] inline bool evaluate(bool condition) noexcept { return condition; }

// Thrown by REP_REQUIRE to abandon the test body that failed a requirement.
// run_all catches it, so an abandoned test is a failed test, not a dead
// process.
struct TestAbort {};

// Records one test.  Suite and name outlive the program (they come from the
// REP_TEST macro as string literals) and registration order is preserved.
int register_test(std::string_view suite, std::string_view name, TestBody body) noexcept;

// Records one passing check.
void note_check() noexcept;
// Records one failing check, with the text of the expression and an optional
// pre-rendered detail block.
void note_failure(std::string_view file, int line, std::string_view expression,
                  std::string_view detail) noexcept;
// Records one failing requirement and abandons the current test.
[[noreturn]] void require_failed(std::string_view file, int line, std::string_view expression,
                                 std::string_view detail);
// Records free-form context for the test that is running.
void note_info(std::string_view message) noexcept;

} // namespace detail

// Runs every registered test, or the tests the command line selects:
//
//   --list           print one "<suite>.<name>" per line, in registration
//                    order, and exit 0
//   --filter <text>  run only the tests whose "<suite>.<name>" contains <text>
//
// Tests run in registration order, so a run is reproducible.  Returns 0 when
// every selected test ran and none of them failed, 1 when a test failed, and 2
// when the command line itself cannot be used.
int run_all(int argc, char** argv);

} // namespace reptest

// Declares one test.  The body follows the macro at file scope.
#define REP_TEST(suite, name)                                                               \
  static void reptest_body_##suite##_##name();                                              \
  namespace {                                                                               \
  [[maybe_unused]] const int reptest_registration_##suite##_##name =                        \
      ::reptest::detail::register_test(#suite, #name, &reptest_body_##suite##_##name);      \
  }                                                                                         \
  static void reptest_body_##suite##_##name()

// Not checked, just reported: context a failure needs to be readable.
#define REP_INFO(message) ::reptest::detail::note_info((message))

// A check that does not stop the test.
#define REP_CHECK(expression)                                                               \
  do {                                                                                      \
    const bool reptest_ok = static_cast<bool>(expression);                                  \
    if (::reptest::detail::evaluate(reptest_ok)) {                                          \
      ::reptest::detail::note_check();                                                      \
    } else {                                                                                \
      ::reptest::detail::note_failure(__FILE__, __LINE__, #expression, "");                 \
    }                                                                                       \
  } while (false)

// A check with an explanation attached to the failure.
#define REP_CHECK_MSG(expression, message)                                                  \
  do {                                                                                      \
    const bool reptest_ok = static_cast<bool>(expression);                                  \
    if (::reptest::detail::evaluate(reptest_ok)) {                                          \
      ::reptest::detail::note_check();                                                      \
    } else {                                                                                \
      ::reptest::detail::note_failure(__FILE__, __LINE__, #expression, (message));          \
    }                                                                                       \
  } while (false)

// Equality and inequality checks.  Both operands are evaluated once, and the
// values are only rendered when the check fails.
#define REP_CHECK_EQ(actual, expected)                                                      \
  do {                                                                                      \
    auto&& reptest_actual = (actual);                                                       \
    auto&& reptest_expected = (expected);                                                   \
    const bool reptest_equal = static_cast<bool>(reptest_actual == reptest_expected);       \
    if (::reptest::detail::evaluate(reptest_equal)) {                                       \
      ::reptest::detail::note_check();                                                      \
    } else {                                                                                \
      ::reptest::detail::note_failure(                                                      \
          __FILE__, __LINE__, #actual " == " #expected,                                     \
          "expected: " + ::reptest::describe(reptest_expected) +                            \
              "\n  actual:   " + ::reptest::describe(reptest_actual));                      \
    }                                                                                       \
  } while (false)

#define REP_CHECK_NE(actual, expected)                                                      \
  do {                                                                                      \
    auto&& reptest_actual = (actual);                                                       \
    auto&& reptest_expected = (expected);                                                   \
    const bool reptest_different = static_cast<bool>(reptest_actual != reptest_expected);   \
    if (::reptest::detail::evaluate(reptest_different)) {                                   \
      ::reptest::detail::note_check();                                                      \
    } else {                                                                                \
      ::reptest::detail::note_failure(                                                      \
          __FILE__, __LINE__, #actual " != " #expected,                                     \
          "expected: not equal to " + ::reptest::describe(reptest_expected) +               \
              "\n  actual:   " + ::reptest::describe(reptest_actual));                      \
    }                                                                                       \
  } while (false)

// Tells a static analyzer that a condition it cannot follow holds from here
// on.  A requirement abandons the test body when it fails, so everything after
// a satisfied requirement may rely on it; without this the analyzer reports a
// possible null dereference at every "require non-null, then use" pair.
#if defined(_PREFAST_)
#define REP_REQUIRE_ESTABLISHES(expression) __analysis_assume(expression)
#else
#define REP_REQUIRE_ESTABLISHES(expression) ((void)0)
#endif

// A requirement abandons the test body when it does not hold.
#define REP_REQUIRE(expression)                                                             \
  do {                                                                                      \
    const bool reptest_ok = static_cast<bool>(expression);                                  \
    if (::reptest::detail::evaluate(reptest_ok)) {                                          \
      REP_REQUIRE_ESTABLISHES(reptest_ok);                                                  \
      ::reptest::detail::note_check();                                                      \
    } else {                                                                                \
      ::reptest::detail::require_failed(__FILE__, __LINE__, #expression, "");               \
    }                                                                                       \
  } while (false)

// A requirement with an explanation attached to the failure.
#define REP_REQUIRE_MSG(expression, message)                                                \
  do {                                                                                      \
    const bool reptest_ok = static_cast<bool>(expression);                                  \
    if (::reptest::detail::evaluate(reptest_ok)) {                                          \
      REP_REQUIRE_ESTABLISHES(reptest_ok);                                                  \
      ::reptest::detail::note_check();                                                      \
    } else {                                                                                \
      ::reptest::detail::require_failed(__FILE__, __LINE__, #expression, (message));        \
    }                                                                                       \
  } while (false)

// An unconditional failure.
#define REP_FAIL(message)                                                                   \
  ::reptest::detail::note_failure(__FILE__, __LINE__, (message), "")

// The one main() in a test binary.
#define REP_TEST_MAIN()                                                                     \
  int main(int argc, char** argv) { return ::reptest::run_all(argc, argv); }

#endif // REP_TESTS_TESTKIT_HPP
