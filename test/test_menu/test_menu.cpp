/**
 * @file test_menu.cpp
 * @brief Regression for live MenuItem gating (slice-0042).
 *
 * A gated list entry must track its predicate every frame, so a peripheral attached after
 * boot un-greys the entry with no menu rebuild -- the submenu-greying desync where the
 * carousel tile lit up but the frozen list item stayed disabled.
 */

#include <unity.h>
#include "ui/components/menu.h"

using namespace adversary;

// Controllable gate the test flips at runtime. A capture-less lambda reads it, mirroring the
// real predicates ([]{ return g_rfidDetected; }) that read detection globals.
static bool s_gateOpen = false;

// Records the actionId the menu dispatched, or -1 if none fired this check.
static int s_firedAction = -1;

static Menu makeMenuWithGatedItem() {
    Menu menu;
    std::vector<MenuItem> items = {
        MenuItem::gated("Dashboard", 42, 't', "Dashboard (Module missing)",
                        []{ return s_gateOpen; }),
        MenuItem::back(),
    };
    menu.setItems(items);
    menu.setOnAction([](int actionId) { s_firedAction = actionId; });
    return menu;
}

void setUp() {
    s_gateOpen = false;
    s_firedAction = -1;
}

void tearDown() {}

// Gate off: the entry is unselectable -- its shortcut does not fire, and the cursor rests on
// the only selectable item (Back), not the gated one. This assertion FAILS before the slice,
// where an ACTION item is selectable regardless of its gate.
void test_gated_item_is_inert_while_gate_closed() {
    s_gateOpen = false;
    Menu menu = makeMenuWithGatedItem();

    // Cursor skipped the gated item (index 0) and landed on Back (index 1).
    TEST_ASSERT_EQUAL_INT(1, menu.getSelection());

    bool handled = menu.handleInput('t');  // the gated item's shortcut
    TEST_ASSERT_FALSE(handled);
    TEST_ASSERT_EQUAL_INT(-1, s_firedAction);
}

// Gate flips open with NO rebuild (no second setItems): the same menu now dispatches the
// action. This is the live behaviour a boot-frozen disabled() item can never give.
void test_gated_item_fires_once_gate_opens_without_rebuild() {
    Menu menu = makeMenuWithGatedItem();

    s_gateOpen = true;  // peripheral now present; menu is NOT rebuilt

    bool handled = menu.handleInput('t');
    TEST_ASSERT_TRUE(handled);
    TEST_ASSERT_EQUAL_INT(42, s_firedAction);
}

// Backward-compat guard: a static disabled() item (no gate) stays unselectable, so the
// isEnabled() default did not change existing greyed entries.
void test_static_disabled_item_stays_unselectable() {
    Menu menu;
    std::vector<MenuItem> items = {
        MenuItem::disabled("Coming Soon"),
        MenuItem::back(),
    };
    menu.setItems(items);
    menu.setOnAction([](int actionId) { s_firedAction = actionId; });

    TEST_ASSERT_EQUAL_INT(1, menu.getSelection());  // skipped the disabled item
    menu.navigateUp();                              // wraps; must not land on the disabled item
    TEST_ASSERT_EQUAL_INT(1, menu.getSelection());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_gated_item_is_inert_while_gate_closed);
    RUN_TEST(test_gated_item_fires_once_gate_opens_without_rebuild);
    RUN_TEST(test_static_disabled_item_stays_unselectable);
    return UNITY_END();
}
