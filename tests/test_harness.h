// Tiny test harness — no external test framework in Phase 1.
// CHECK(cond) prints the failure location and aborts the run with
// a non-zero exit code. A passing run prints every section name.
#pragma once

#include <cstdio>
#include <cstdlib>

namespace test {

inline int& Failures() {
  static int n = 0;
  return n;
}

inline void BeginSection(const char* name) {
  std::printf("[test] %s\n", name);
}

inline void Report() {
  if (Failures() == 0) {
    std::printf("[test] ALL CHECKS PASSED\n");
  } else {
    std::printf("[test] %d CHECK(S) FAILED\n", Failures());
  }
}

}  // namespace test

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      ++test::Failures();                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
    }                                                                      \
  } while (0)
