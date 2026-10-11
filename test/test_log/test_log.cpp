/**
 * @file test_log.cpp
 * @brief Gate tests for the logging layer at INFO (slice-0052, ADR-0051).
 *
 * The gate is compile-time (ADV_LOG_LEVEL fixes it per translation unit), so each boundary
 * needs its own TU/binary. This one pins INFO: error/warn/info emit, debug is stripped.
 * test_log_quiet pins ERROR for the opposite boundary.
 */

#include <unity.h>
#include <string>

// Pin this TU at INFO before the layer is included, overriding the env's -DADV_LOG_LEVEL so
// the test states the level it exercises. 3 == ADV_LOG_INFO (the ordinals live in log.h).
#undef ADV_LOG_LEVEL
#define ADV_LOG_LEVEL 3
#include "utils/log.h"
#include "../common/arduino_mocks.h"

void setUp() { mockSerialClear(); }
void tearDown() {}

// At INFO, the three always-or-at-level macros emit their tagged line to the captured Serial.
void test_info_build_emits_error_warn_info() {
    LOG_E("T", "e%d", 1);
    LOG_W("T", "w");
    LOG_I("T", "i");
    TEST_ASSERT_TRUE(g_mockSerialLog.find("[ERROR] T: e1") != std::string::npos);
    TEST_ASSERT_TRUE(g_mockSerialLog.find("[WARN] T: w") != std::string::npos);
    TEST_ASSERT_TRUE(g_mockSerialLog.find("[INFO] T: i") != std::string::npos);
}

// At INFO, LOG_D compiles to a no-op: nothing is emitted (its format string left rodata).
void test_info_build_strips_debug() {
    LOG_D("T", "should not appear");
    TEST_ASSERT_TRUE(g_mockSerialLog.empty());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_info_build_emits_error_warn_info);
    RUN_TEST(test_info_build_strips_debug);
    return UNITY_END();
}
