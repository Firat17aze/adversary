/**
 * @file test_list_scroll_model.cpp
 * @brief Regression for the shared list selection/scroll model (slice-0062).
 *
 * Each test encodes a rule a hand-rolled list previously got wrong. The two headline bugs:
 *  - the wardriving off-screen selection (scroll window wider than the rendered rows, 158246b)
 *    -> test_down_on_last_visible_row_advances_window / test_up_scrolls_window_back_into_view;
 *  - the attack-config premature scroll (hardcoded visible-count below the rendered rows,
 *    a2417c8) -> test_count_fits_viewport_never_scrolls / test_shrinking_viewport_reclamps.
 * Plus the wrap policy and the scrollbar-thumb geometry the component now owns.
 */

#include <unity.h>
#include "ui/components/list_scroll_model.h"

using namespace adversary;

// A list of 10 items in a 4-row viewport — the canonical "more than fits" case.
static ListScrollModel makeList(int count, int viewport) {
    ListScrollModel model;
    model.setItemCount(count);
    model.setViewport(viewport);
    return model;
}

void setUp() {}
void tearDown() {}

// --- scroll window follows the selection -------------------------------------------------

void test_down_on_last_visible_row_advances_window() {
    ListScrollModel m = makeList(10, 4);  // rows 0..3 visible, selection 0
    m.moveDown(); m.moveDown(); m.moveDown();  // selection 3, still the last visible row
    TEST_ASSERT_EQUAL_INT(3, m.selectedIndex());
    TEST_ASSERT_EQUAL_INT(0, m.scrollOffset());

    m.moveDown();  // selection 4 — would be off-screen; window must advance
    TEST_ASSERT_EQUAL_INT(4, m.selectedIndex());
    TEST_ASSERT_EQUAL_INT(1, m.scrollOffset());
    // The selection is inside [offset, offset+viewport).
    TEST_ASSERT_TRUE(m.selectedIndex() >= m.scrollOffset());
    TEST_ASSERT_TRUE(m.selectedIndex() < m.scrollOffset() + m.visibleRows());
}

void test_up_scrolls_window_back_into_view() {
    ListScrollModel m = makeList(10, 4);
    m.setSelectedIndex(6);  // forces window to 3..6 (offset 3)
    TEST_ASSERT_EQUAL_INT(3, m.scrollOffset());

    m.moveUp(); m.moveUp(); m.moveUp();  // selection 3 — still visible, no scroll yet
    TEST_ASSERT_EQUAL_INT(3, m.selectedIndex());
    TEST_ASSERT_EQUAL_INT(3, m.scrollOffset());

    m.moveUp();  // selection 2 — above the window, it must follow up
    TEST_ASSERT_EQUAL_INT(2, m.selectedIndex());
    TEST_ASSERT_EQUAL_INT(2, m.scrollOffset());
}

// --- a list that fits never scrolls ------------------------------------------------------

void test_count_fits_viewport_never_scrolls() {
    ListScrollModel m = makeList(4, 7);  // 4 items, 7 rows fit — nothing is off-screen
    TEST_ASSERT_FALSE(m.isScrollable());
    for (int i = 0; i < 6; ++i) m.moveDown();  // churn past the end a few times
    TEST_ASSERT_EQUAL_INT(0, m.scrollOffset());
    TEST_ASSERT_FALSE(m.isScrollable());
}

void test_shrinking_viewport_reclamps_offset() {
    ListScrollModel m = makeList(10, 7);
    m.setSelectedIndex(9);            // bottom; offset becomes 3 (3..9)
    TEST_ASSERT_EQUAL_INT(3, m.scrollOffset());

    m.setViewport(4);                 // layout now only draws 4 rows
    // Selection 9 must still be visible: offset re-clamps to 6 (6..9).
    TEST_ASSERT_EQUAL_INT(6, m.scrollOffset());
    TEST_ASSERT_TRUE(m.selectedIndex() < m.scrollOffset() + m.visibleRows());
}

// --- wrap at the ends --------------------------------------------------------------------

void test_up_on_first_wraps_to_last_and_shows_it() {
    ListScrollModel m = makeList(10, 4);  // selection 0, offset 0
    m.moveUp();                            // wrap to item 9
    TEST_ASSERT_EQUAL_INT(9, m.selectedIndex());
    // The wrapped selection must be on screen: offset 6 shows rows 6..9.
    TEST_ASSERT_EQUAL_INT(6, m.scrollOffset());
    TEST_ASSERT_TRUE(m.selectedIndex() < m.scrollOffset() + m.visibleRows());
}

void test_down_on_last_wraps_to_first() {
    ListScrollModel m = makeList(10, 4);
    m.setSelectedIndex(9);                 // bottom
    TEST_ASSERT_EQUAL_INT(6, m.scrollOffset());
    m.moveDown();                          // wrap to item 0
    TEST_ASSERT_EQUAL_INT(0, m.selectedIndex());
    TEST_ASSERT_EQUAL_INT(0, m.scrollOffset());
}

// --- scrollbar thumb geometry ------------------------------------------------------------

void test_thumb_height_is_proportional_with_minimum() {
    ListScrollModel big = makeList(100, 5);  // visible fraction 5/100 -> 5px of a 100px track
    // Proportional would be 5px, below the 6px floor, so it clamps up to the minimum.
    TEST_ASSERT_EQUAL_INT(ListScrollModel::MIN_THUMB_PX, big.thumbHeightPx(100));

    ListScrollModel half = makeList(10, 5);  // half the list fits -> half the track
    TEST_ASSERT_EQUAL_INT(50, half.thumbHeightPx(100));

    // A list that fits gets the full track (renderer won't draw it, but the math is defined).
    ListScrollModel fits = makeList(3, 5);
    TEST_ASSERT_EQUAL_INT(100, fits.thumbHeightPx(100));
}

void test_thumb_top_tracks_offset() {
    ListScrollModel m = makeList(10, 5);  // maxOffset 5, thumb height 50 on a 100px track
    TEST_ASSERT_EQUAL_INT(0, m.thumbTopPx(100));        // top of list -> top of track

    m.setSelectedIndex(9);                               // bottom -> offset 5 (max)
    TEST_ASSERT_EQUAL_INT(5, m.scrollOffset());
    // travel = 100 - 50 = 50, at max offset the thumb sits flush with the track bottom.
    TEST_ASSERT_EQUAL_INT(50, m.thumbTopPx(100));
    TEST_ASSERT_TRUE(m.thumbTopPx(100) + m.thumbHeightPx(100) <= 100);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_down_on_last_visible_row_advances_window);
    RUN_TEST(test_up_scrolls_window_back_into_view);
    RUN_TEST(test_count_fits_viewport_never_scrolls);
    RUN_TEST(test_shrinking_viewport_reclamps_offset);
    RUN_TEST(test_up_on_first_wraps_to_last_and_shows_it);
    RUN_TEST(test_down_on_last_wraps_to_first);
    RUN_TEST(test_thumb_height_is_proportional_with_minimum);
    RUN_TEST(test_thumb_top_tracks_offset);
    return UNITY_END();
}
