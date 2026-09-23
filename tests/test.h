#pragma once
// Minimal test framework. No dependencies: tests self-register and the runner in
// test_main.cpp executes them in registration order (file order is stable because
// CMake globs tests/*.cpp in sorted order).

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace hoi_test {

struct TestCase {
    const char* file;
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* file, const char* name, std::function<void()> fn) {
        registry().push_back(TestCase{file, name, std::move(fn)});
    }
};

struct Failure {
    std::string message;
};

inline void fail(const char* file, int line, const std::string& message) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s:%d: %s", file, line, message.c_str());
    throw Failure{buf};
}

}  // namespace hoi_test

#define HOI_TEST(name)                                                                  \
    static void hoi_test_fn_##name();                                                   \
    static ::hoi_test::Registrar hoi_test_reg_##name(__FILE__, #name, hoi_test_fn_##name); \
    static void hoi_test_fn_##name()

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) ::hoi_test::fail(__FILE__, __LINE__, "CHECK failed: " #cond);      \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        auto&& _a = (a);                                                                \
        auto&& _b = (b);                                                                \
        if (!(_a == _b)) {                                                              \
            ::hoi_test::fail(__FILE__, __LINE__,                                        \
                             std::string("CHECK_EQ failed: " #a " != " #b));            \
        }                                                                               \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                           \
    do {                                                                                \
        double _a = static_cast<double>(a);                                             \
        double _b = static_cast<double>(b);                                             \
        if (std::fabs(_a - _b) > (eps)) {                                               \
            char _msg[256];                                                             \
            std::snprintf(_msg, sizeof(_msg), "CHECK_NEAR failed: " #a "=%.6f vs " #b   \
                                              "=%.6f (eps %.6g)",                       \
                          _a, _b, static_cast<double>(eps));                            \
            ::hoi_test::fail(__FILE__, __LINE__, _msg);                                 \
        }                                                                               \
    } while (0)

#define CHECK_GT(a, b)                                                                  \
    do {                                                                                \
        if (!((a) > (b))) ::hoi_test::fail(__FILE__, __LINE__, "CHECK_GT failed: " #a " > " #b); \
    } while (0)

#define CHECK_LT(a, b)                                                                  \
    do {                                                                                \
        if (!((a) < (b))) ::hoi_test::fail(__FILE__, __LINE__, "CHECK_LT failed: " #a " < " #b); \
    } while (0)
