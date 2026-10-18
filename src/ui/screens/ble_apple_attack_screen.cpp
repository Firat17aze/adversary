/**
 * @file ble_apple_attack_screen.cpp
 * @brief BleAppleAttackScreen implementation
 */

#include "ble_apple_attack_screen.h"
#include "ui/theme.h"
#include "ui/components/status_bar.h"
#include "modules/system/system_manager.h"
#include "modules/ble/ble_scanner.h"

namespace adversary {

BleAppleAttackScreen::BleAppleAttackScreen()
    : m_state(State::CONFIG)
    , m_selectedPrompt(BleSpamPrompt::PAIRING)
    , m_active(false)
    , m_shouldExit(false)
    , m_needsRedraw(true)
    , m_pulseStartTime(0)
{
    m_promptOptions = {
        {BleSpamPrompt::PAIRING, "AirPods/Beats (Pairing)"},
        {BleSpamPrompt::APPLE_ID_MODAL, "Apple ID Required (Modal)"},
        {BleSpamPrompt::SOFTWARE_UPDATE, "Software Update (Modal)"},
        {BleSpamPrompt::TRANSFER_PHONE, "Transfer Phone (Modal)"},
        {BleSpamPrompt::GUIDED_ACCESS, "Guided Access (Modal)"}
    };
    m_promptList.setItemCount((int)m_promptOptions.size());
}

void BleAppleAttackScreen::show() {
    m_active = true;
    m_shouldExit = false;
    m_needsRedraw = true;
    m_state = State::CONFIG;
    
    footerHints_.setHints({
        {'\n', "Start", true},
        {'`', "Back", true}
    });
}

void BleAppleAttackScreen::hide() {
    stopAttack();
    m_active = false;
    BLESpanner::getInstance().forceRelease();
    BLEScanner::getInstance().deinit();
}

void BleAppleAttackScreen::update() {
    if (!m_active) return;
    
    if (m_state == State::ATTACK) {
        m_needsRedraw = true; // Periodic redraw for telemetry
    }
}

void BleAppleAttackScreen::render(Canvas& canvas) {
    if (!m_needsRedraw) return;
    m_needsRedraw = false;

    canvas.fillScreen(theme::BG_PRIMARY());
    
    if (m_state == State::CONFIG) {
        drawConfig(canvas);
    } else {
        drawAttack(canvas);
    }
    
    footerHints_.render(canvas);
}

void BleAppleAttackScreen::drawConfig(Canvas& canvas) {
    ui::StatusBar::render(canvas, "APPLE ATTACK", "CONFIG");
    
    int y = 30;
    canvas.setTextColor(theme::TEXT_SECONDARY());
    canvas.setCursor(5, y);
    canvas.print("Select Attack Type:");
    y += 12;

    // The shared component paints the selected-row band (dimmed to BG_SECONDARY when the footer
    // holds focus) and the thumb; this callback draws one option's label only (slice-0063/0065).
    m_promptList.setItemCount((int)m_promptOptions.size());
    const int16_t listH = (int16_t)(m_promptOptions.size() * theme::LIST_ROW_HEIGHT);
    ScrollableList::render(canvas, m_promptList, 0, (int16_t)y, config::SCREEN_WIDTH, listH,
        theme::LIST_ROW_HEIGHT, [this](Canvas& c, int idx, int16_t rowY, bool /*selected*/) {
            c.setTextColor(theme::TEXT_PRIMARY());
            c.setCursor(10, rowY + 2);
            c.print(m_promptOptions[idx].label);
        },
        !footerHints_.hasFocus());
}

void BleAppleAttackScreen::drawAttack(Canvas& canvas) {
    ui::StatusBar::render(canvas, "APPLE ATTACK", "RUNNING");
    
    const auto& spanner = BLESpanner::getInstance();
    
    int centerX = config::SCREEN_WIDTH / 2;
    int centerY = config::SCREEN_HEIGHT / 2 - 10;
    
    // Draw BLE Icon with pulse
    uint32_t elapsed = millis() - m_pulseStartTime;
    int radius = 15 + (elapsed % 1000) / 50;
    canvas.drawCircle(centerX, centerY, radius, theme::ACCENT());
    canvas.fillCircle(centerX, centerY, 10, theme::ACCENT());
    
    // Telemetry
    int y = centerY + 30;
    canvas.setTextColor(theme::TEXT_PRIMARY());
    canvas.setCursor(5, y);
    canvas.printf("Pkts: %u", (unsigned int)spanner.getPacketCount());
    
    canvas.setCursor(config::SCREEN_WIDTH / 2, y);
    canvas.printf("PPS: %.1f", spanner.getPPS());
    
    y += 12;
    canvas.setTextColor(theme::TEXT_SECONDARY());
    canvas.setCursor(5, y);
    canvas.printf("Type: %s", m_promptOptions[m_promptList.selectedIndex()].label);
}

void BleAppleAttackScreen::startAttack() {
    m_state = State::ATTACK;
    m_pulseStartTime = millis();
    
    BleSpamConfig cfg;
    cfg.provider = BleSpamProvider::APPLE;
    cfg.prompt = m_promptOptions[m_promptList.selectedIndex()].prompt;
    cfg.intensity = 10;
    
    BLESpanner::getInstance().start(cfg);
    
    footerHints_.setHints({
        {'\n', "Stop", true},
        {'`', "Back", true}
    });
}

void BleAppleAttackScreen::stopAttack() {
    BLESpanner::getInstance().stop();
    m_state = State::CONFIG;
    
    footerHints_.setHints({
        {'\n', "Start", true},
        {'`', "Back", true}
    });
}

bool BleAppleAttackScreen::handleInput(char key) {
    // Footer Focus Dispatch
    char footerAction = 0;
    if (footerHints_.handleInputWithDispatch(key, footerAction)) {
        if (footerAction) return handleInput(footerAction);
        m_needsRedraw = true;
        return true;
    }

    switch (key) {
        case ';': // UP
        case 'k':
            if (m_state == State::CONFIG) {
                m_promptList.setItemCount((int)m_promptOptions.size());
                m_promptList.moveUp();  // wraps to last; model keeps the selection on screen
                m_needsRedraw = true;
            }
            return true;

        case '.': // DOWN
        case 'j':
            if (m_state == State::CONFIG) {
                m_promptList.setItemCount((int)m_promptOptions.size());
                m_promptList.moveDown();  // wraps to first; model keeps the selection on screen
                m_needsRedraw = true;
            }
            return true;
            
        case '\n': // ENTER
            if (m_state == State::CONFIG) {
                startAttack();
            } else {
                stopAttack();
            }
            m_needsRedraw = true;
            return true;
            
        case '`': // ESC
            if (m_state == State::ATTACK) {
                stopAttack();
            } else {
                m_shouldExit = true;
            }
            return true;
    }
    return false;
}

} // namespace adversary
