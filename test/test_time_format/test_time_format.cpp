/**
 * @file test_time_format.cpp
 * @brief Unit tests for duration/time formatting helpers (slice-0057)
 */

#include <unity.h>
#include <cstring>
#include "utils/time_format.h"

using namespace adversary::utils;

void setUp() {}
void tearDown() {}

/** Short durations keep the two-digit zero-padded HH:MM:SS shape. */
void test_duration_padded(void) {
    char out[16];
    formatDuration(3661, out, sizeof(out));  // 1h 1m 1s
    TEST_ASSERT_EQUAL_STRING("01:01:01", out);
}

void test_duration_zero(void) {
    char out[16];
    formatDuration(0, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("00:00:00", out);
}

/**
 * Hours grow past two digits without truncation. 444441600 s == 123456 h exactly, a
 * 12-char result ("123456:00:00") that the screen's old char[12] buffer could not hold —
 * this is the regression for the -Wformat-truncation overflow (slice-0057 DoD scenario 1).
 */
void test_duration_hours_overflow_old_buffer(void) {
    char out[16];
    formatDuration(444441600UL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("123456:00:00", out);
}

/** The uint32_t worst case still fits a 16-byte buffer. */
void test_duration_uint32_max(void) {
    char out[16];
    formatDuration(4294967295UL, out, sizeof(out));  // 1193046 h 28 m 15 s
    TEST_ASSERT_EQUAL_STRING("1193046:28:15", out);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_duration_padded);
    RUN_TEST(test_duration_zero);
    RUN_TEST(test_duration_hours_overflow_old_buffer);
    RUN_TEST(test_duration_uint32_max);
    return UNITY_END();
}
