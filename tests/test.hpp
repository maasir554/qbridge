#ifndef QBRIDGE_TEST_HPP
#define QBRIDGE_TEST_HPP

#include <cstdio>
#include <cstdlib>

extern int qb_test_failures;

#define CHECK(expression) do {                                             \
    if (!(expression)) {                                                   \
        std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,       \
                     __LINE__, #expression);                               \
        ++qb_test_failures;                                                \
    }                                                                      \
} while (0)

void test_protocol();

#endif

