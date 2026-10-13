#pragma once

/**
 * @file time_format.h
 * @brief Pure time/duration string formatting helpers.
 */

#include <stdint.h>
#include <cstddef>
#include <cstdio>

namespace adversary {
namespace utils {

/**
 * @brief Format a duration as "HH:MM:SS" into @p out.
 *
 * Hours carry a two-digit minimum width (so short durations read "01:02:03") but are not
 * capped — a long or corrupt @p totalSeconds grows the hours field rather than truncating.
 * The caller must size @p out for the worst case: a uint32_t duration is at most
 * "1193046:59:59" (14 bytes incl. NUL), so a 16-byte buffer is safe.
 *
 * Pure (no clock reads), so it is unit-testable off-hardware.
 *
 * @param totalSeconds Duration in seconds
 * @param out          Output buffer (NUL-terminated); size it >= 16 for any uint32_t input
 * @param outLen       Size of @p out
 */
inline void formatDuration(uint32_t totalSeconds, char* out, size_t outLen) {
    const uint32_t hours = totalSeconds / 3600;
    const uint32_t minutes = (totalSeconds % 3600) / 60;
    const uint32_t seconds = totalSeconds % 60;
    snprintf(out, outLen, "%02lu:%02lu:%02lu",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
}

} // namespace utils
} // namespace adversary
