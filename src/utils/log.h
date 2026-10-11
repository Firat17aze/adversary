#pragma once

/**
 * @file log.h
 * @brief Project-wide, compile-time level-gated logging layer (ADR-0051).
 *
 * One front door for all firmware logging: LOG_E / LOG_W / LOG_I / LOG_D(tag, fmt, ...).
 * The verbosity is fixed at build time by ADV_LOG_LEVEL. A macro whose level is above
 * ADV_LOG_LEVEL expands to an empty statement, so its format string is never emitted to
 * the image's rodata -- this is the mechanism by which a quieter build recovers flash
 * (ADR-0051). Errors and warnings are meant to always ship; only INFO and DEBUG are the
 * strippable levels, so a gated build can never silently drop an error (fail-loud).
 *
 * `tag` and `fmt` must both be string literals: the macros concatenate them into a single
 * printf format at compile time (the idiom the retired wardriving LOG_ macros used). Every
 * call names its module with a short literal tag, e.g. LOG_I("WARDRV", "aps=%d", count).
 *
 * Output goes through Serial. On the native test target Serial is the MockSerial that
 * captures into g_mockSerialLog, so the gate is assertable in a unit test. LOG_E tees to
 * the SD ErrorLogger on device (adversary::logger::logError), making error persistence the
 * default instead of a per-site accident; the native target has no SD, so there LOG_E just
 * prints the same line to the captured Serial -- the SD half is device-verified.
 */

#include <Arduino.h>
#include <cstdio>
#include "utils/error_logger.h"

// Level ordinals, ascending verbosity. A build sets ADV_LOG_LEVEL to one of these.
#define ADV_LOG_NONE  0
#define ADV_LOG_ERROR 1
#define ADV_LOG_WARN  2
#define ADV_LOG_INFO  3
#define ADV_LOG_DEBUG 4

#ifndef ADV_LOG_LEVEL
#define ADV_LOG_LEVEL ADV_LOG_INFO
#endif

// On device, LOG_E formats into a fixed stack buffer before handing the text to the SD
// ErrorLogger, which takes a plain string rather than a printf format. Errors are short
// diagnostics, so this is ample and keeps the path off the heap; a longer message truncates.
#define ADV_LOG_ERROR_MSG_MAX 160

#if ADV_LOG_LEVEL >= ADV_LOG_ERROR
  #ifdef NATIVE_BUILD
    // No SD on native: print the same line the device logs, to the captured Serial.
    #define LOG_E(tag, fmt, ...) \
        do { Serial.printf("[ERROR] " tag ": " fmt "\n", ##__VA_ARGS__); } while (0)
  #else
    #define LOG_E(tag, fmt, ...)                                               \
        do {                                                                   \
            char _advLogMsg[ADV_LOG_ERROR_MSG_MAX];                            \
            std::snprintf(_advLogMsg, sizeof(_advLogMsg), fmt, ##__VA_ARGS__); \
            ::adversary::logger::logError(tag, _advLogMsg);                    \
        } while (0)
  #endif
#else
  #define LOG_E(tag, fmt, ...) do {} while (0)
#endif

#if ADV_LOG_LEVEL >= ADV_LOG_WARN
  #define LOG_W(tag, fmt, ...) \
      do { Serial.printf("[WARN] " tag ": " fmt "\n", ##__VA_ARGS__); } while (0)
#else
  #define LOG_W(tag, fmt, ...) do {} while (0)
#endif

#if ADV_LOG_LEVEL >= ADV_LOG_INFO
  #define LOG_I(tag, fmt, ...) \
      do { Serial.printf("[INFO] " tag ": " fmt "\n", ##__VA_ARGS__); } while (0)
#else
  #define LOG_I(tag, fmt, ...) do {} while (0)
#endif

#if ADV_LOG_LEVEL >= ADV_LOG_DEBUG
  #define LOG_D(tag, fmt, ...) \
      do { Serial.printf("[DEBUG] " tag ": " fmt "\n", ##__VA_ARGS__); } while (0)
#else
  #define LOG_D(tag, fmt, ...) do {} while (0)
#endif
