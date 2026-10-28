// SPDX-License-Identifier: AGPL-3.0-or-later
// Minimal test helpers, no dependency.
// A failed check prints file:line and the values, then the run continues;
// main() returns the number of failures (0 = success for ctest).
#pragma once

#include <cmath>
#include <cstdio>

namespace check {

inline int failures = 0;
inline int passed   = 0;

inline void near(double got, double want, double tol, const char* expr, const char* file, int line) {
    if (std::abs(got - want) <= tol) { ++passed; return; }
    ++failures;
    std::printf("FAIL %s:%d  %s\n     got %.10g, want %.10g (tol %.3g)\n", file, line, expr, got, want, tol);
}

inline void truth(bool ok, const char* expr, const char* file, int line) {
    if (ok) { ++passed; return; }
    ++failures;
    std::printf("FAIL %s:%d  %s\n", file, line, expr);
}

inline int summary() {
    std::printf("%d checks passed, %d failed\n", passed, failures);
    return failures;
}

} // namespace check

#define CHECK_NEAR(got, want, tol) check::near((got), (want), (tol), #got, __FILE__, __LINE__)
#define CHECK(cond)                check::truth((cond), #cond, __FILE__, __LINE__)
