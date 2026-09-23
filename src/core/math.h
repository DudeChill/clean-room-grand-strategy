#pragma once
// Deterministic math helpers.
//
// Only operations with exactly specified IEEE-754 behaviour are used in the
// simulation. Transcendental functions are avoided on hot deterministic paths;
// where they are required the call site is documented and hashed output will
// detect cross-build drift.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace hoi {

template <typename T>
[[nodiscard]] constexpr T clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

[[nodiscard]] constexpr double clamp01(double v) { return clamp(v, 0.0, 1.0); }

[[nodiscard]] constexpr double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Division that never produces NaN/Inf: zero (or near-zero) denominator yields 0.
[[nodiscard]] inline double safe_div(double num, double den) {
    if (den <= 0.0 || den != den) return 0.0;
    return num / den;
}

[[nodiscard]] inline double finite_or(double v, double fallback) {
    return std::isfinite(v) ? v : fallback;
}

[[nodiscard]] constexpr bool nearly_equal(double a, double b, double eps = 1e-9) {
    double d = a - b;
    if (d < 0) d = -d;
    return d <= eps;
}

// Integer power of two check helpers used by map/state bucketing.
[[nodiscard]] constexpr uint32_t next_pow2(uint32_t v) {
    if (v <= 1) return 1;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

}  // namespace hoi
