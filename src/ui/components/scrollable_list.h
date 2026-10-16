/**
 * @file scrollable_list.h
 * @brief Device-side renderer for a scrolling selection list (slice-0062).
 *
 * The thin display half of the shared list component. It owns no state: the screen keeps the
 * pure ListScrollModel (so handleInput can drive it), and this renderer, each frame,
 *   1. derives the viewport height from the live layout rect — the single source of truth for
 *      how many rows fit, which the model's scroll window then honours;
 *   2. calls the screen's per-row draw callback for each visible item (row appearance stays
 *      with the screen — selection highlight, columns, colours);
 *   3. draws the one unified scrollbar thumb when the list is scrollable.
 *
 * This replaces four divergent hand-rolled scrollbar idioms (thin bar, proportional thumb,
 * up/down triangles, "N/total" text) with a single proportional thumb. render() is a template
 * on the canvas type and is compiled on device only, mirroring Menu::render / StatusBar.
 */

#pragma once

#include <cstdint>
#include "list_scroll_model.h"

namespace adversary {

class ScrollableList {
public:
    /// Scrollbar thumb width in pixels. The one unified value; thumb *height* geometry lives
    /// in ListScrollModel (it is pure math and unit-tested), the width is purely cosmetic.
    static constexpr int16_t SCROLLBAR_WIDTH_PX = 4;

    /**
     * @brief Render the visible rows and the scrollbar for a list.
     * @param model      the screen's persistent selection/scroll state (item count kept in sync
     *                   by the screen); its viewport is set here from the rect.
     * @param x,y,w,h    the list content rect in canvas pixels.
     * @param rowHeight  pixel height of one row; visibleRows = h / rowHeight.
     * @param drawRow    callback drawing one row: (canvas, itemIndex, rowTopY, isSelected).
     */
    template<typename Canvas, typename DrawRow>
    static void render(Canvas& canvas, ListScrollModel& model,
                       int16_t x, int16_t y, int16_t w, int16_t h,
                       int16_t rowHeight, DrawRow drawRow);
};

} // namespace adversary

// Template implementation is device-only (it touches the canvas and the theme), matching the
// other native-compiled components whose render() is ESP32-guarded (see menu.h).
#ifdef ESP32
#include "../theme.h"

namespace adversary {

template<typename Canvas, typename DrawRow>
void ScrollableList::render(Canvas& canvas, ListScrollModel& model,
                            int16_t x, int16_t y, int16_t w, int16_t h,
                            int16_t rowHeight, DrawRow drawRow) {
    const int visibleRows = rowHeight > 0 ? h / rowHeight : 0;
    model.setViewport(visibleRows);

    const int offset = model.scrollOffset();
    const int selected = model.selectedIndex();
    for (int row = 0; row < visibleRows && (offset + row) < model.itemCount(); ++row) {
        const int itemIndex = offset + row;
        const int16_t rowTopY = y + static_cast<int16_t>(row * rowHeight);
        drawRow(canvas, itemIndex, rowTopY, itemIndex == selected);
    }

    if (model.isScrollable()) {
        const int16_t thumbHeight = static_cast<int16_t>(model.thumbHeightPx(h));
        const int16_t thumbTop = y + static_cast<int16_t>(model.thumbTopPx(h));
        canvas.fillRect(x + w - SCROLLBAR_WIDTH_PX, thumbTop,
                        SCROLLBAR_WIDTH_PX, thumbHeight, theme::ACCENT());
    }
}

} // namespace adversary
#endif // ESP32
