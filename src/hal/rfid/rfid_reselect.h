#pragma once

#include <MFRC522_I2C.h>

namespace adversary {

/**
 * @brief Wake a MIFARE tag into a known, selected (ACTIVE) state.
 *
 * The one re-select used by every full-select site (the writer's block I/O and the audit's initial,
 * per-sector and read-path selects). A WS1850S/clone leaves a tag ACTIVE after an auth, and an
 * ACTIVE tag ignores WUPA until it is HALTed (field-observed on 23FF9C1E, slice-0046); the cold
 * field and the Gen1a backdoor wake are also timing-sensitive (slice-0044). So each attempt halts
 * to a known state, wakes with WUPA (REQA fallback for a tag that was never halted), then runs
 * anticollision+select, retrying a few times before giving up.
 *
 * @return true once the tag is selected; false if it did not respond within RESELECT_ATTEMPTS.
 */
bool reselectCard(MFRC522_I2C* rfid);

} // namespace adversary
