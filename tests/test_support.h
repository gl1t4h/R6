#pragma once

#include <cstdio>

namespace r6::test {

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline void check(bool condition, const char* expression, const char* file,
                  int line) {
    if (condition) return;
    std::printf("FAIL %s:%d: %s\n", file, line, expression);
    ++failure_count();
}

}  // namespace r6::test

#define R6_CHECK(expression) \
    ::r6::test::check((expression), #expression, __FILE__, __LINE__)
