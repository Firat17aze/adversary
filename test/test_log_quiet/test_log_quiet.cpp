/**
 * @file test_log_quiet.cpp
 * @brief Gate test for the logging layer at ERROR (slice-0052, ADR-0051).
 *
 * Pins the quietest non-silent level to prove the floor: an error is never stripped, while
 * warn/info/debug are all gated away. Paired with test_log, which pins INFO. Separate TU
 * because ADV_LOG_LEVEL is a compile-time constant — one level per translation unit.
 */

#include <unity.h>
#include <string>

// 1 == ADV_LOG_ERROR (ordinals in log.h). Override the env default before including the layer.
#undef ADV_LOG_LEVEL
#define ADV_LOG_LEVEL 1
#include "utils/log.h"
#include "../common/arduino_mocks.h"

void setUp() { mockSerialClear(); }
void tearDown() {}

// At ERROR, only LOG_E survives; warn/info/debug are compiled out, so fail-loud is intact
// (an error can never be silently gated) while all chattier levels leave the image.
void test_error_build_emits_only_error() {
    LOG_E("T", "boom");
    LOG_W("T", "w");
    LOG_I("T", "i");
    LOG_D("T", "d");
    TEST_ASSERT_TRUE(g_mockSerialLog.find("[ERROR] T: boom") != std::string::npos);
    TEST_ASSERT_TRUE(g_mockSerialLog.find("[WARN]") == std::string::npos);
    TEST_ASSERT_TRUE(g_mockSerialLog.find("[INFO]") == std::string::npos);
    TEST_ASSERT_TRUE(g_mockSerialLog.find("[DEBUG]") == std::string::npos);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_error_build_emits_only_error);
    return UNITY_END();
}
