// Minimal host test helpers: CHECK(cond) counts failures, main returns non zero on any failure.
#pragma once
#include <cstdio>

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

#define TEST_MAIN_END()                                                      \
    std::printf("%d checks, %d failures\n", g_checks, g_failures);           \
    return g_failures ? 1 : 0
