// Test runner. Exits non-zero when any test fails so CI and the release gate can
// rely on the exit code.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test.h"

namespace hoi_test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

}  // namespace hoi_test

int main(int argc, char** argv) {
    const char* filter = nullptr;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            list = true;
        } else if (std::strncmp(argv[i], "--filter=", 9) == 0) {
            filter = argv[i] + 9;
        }
    }

    auto& tests = hoi_test::registry();
    if (list) {
        for (const auto& t : tests) std::printf("%s\n", t.name);
        return 0;
    }

    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;
    for (const auto& t : tests) {
        if (filter && std::strstr(t.name, filter) == nullptr) continue;
        try {
            t.fn();
            ++passed;
            std::printf("PASS %s\n", t.name);
        } catch (const hoi_test::Failure& f) {
            ++failed;
            failures.push_back(std::string(t.name) + ": " + f.message);
            std::printf("FAIL %s\n     %s\n", t.name, f.message.c_str());
        } catch (const std::exception& e) {
            ++failed;
            failures.push_back(std::string(t.name) + ": exception: " + e.what());
            std::printf("FAIL %s\n     exception: %s\n", t.name, e.what());
        }
    }

    std::printf("\n%d passed, %d failed, %d total\n", passed, failed, passed + failed);
    for (const auto& f : failures) std::printf("  FAILED: %s\n", f.c_str());
    return failed == 0 ? 0 : 1;
}
