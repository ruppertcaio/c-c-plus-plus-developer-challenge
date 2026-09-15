#include "test.h"

TEST(check_and_eq_pass) {
    CHECK(1 + 1 == 2);
    CHECK_EQ(40 + 2, 42);
}

TEST(mem_pass) {
    static const unsigned char a[] = { 1, 2, 3, 4 };
    static const unsigned char b[] = { 1, 2, 3, 4 };
    CHECK_MEM(a, b, sizeof(a));
}

int main(void) {
    RUN_TEST(check_and_eq_pass);
    RUN_TEST(mem_pass);
    return TEST_SUMMARY();
}
