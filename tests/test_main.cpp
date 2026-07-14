#include "test.hpp"

int qb_test_failures = 0;

int main() {
    test_protocol();
    if (qb_test_failures) {
        std::fprintf(stderr, "%d test assertion(s) failed\n", qb_test_failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}

