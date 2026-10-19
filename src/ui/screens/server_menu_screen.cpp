/**
 * @file server_menu_screen.cpp
 * @brief Server Mode submenu implementation
 */

#include "server_menu_screen.h"
#include "../theme.h"
#include "modules/server/server_manager.h"
#include "../screen_manager.h"

#ifdef ESP32
#include <Arduino.h>
#include "utils/log.h"
#endif

// Canvas management for server mode (free 64KB for network operations)
extern void adversary_ui_purge_canvas();
extern void adversary_ui_restore_canvas();
extern void adversary_ui_render_forced();

namespace adversary {

ServerMenuScreen::ServerMenuScreen()
    : visible_(false)
    , shouldExit_(false)
    , needsRedraw_(true)
    , serverRunning_(false)
{
}

ServerMenuScreen::~ServerMenuScreen() {
    deinit();
}

void ServerMenuScreen::init() {
    menuList_.setItemCount(MENU_ITEMS);
    menuList_.setSelectedIndex(0);
    needsRedraw_ = true;
    serverRunning_ = ServerManager::getInstance().isRunning();
    
    // Update menu labels based on server state
    if (serverRunning_) {
        menuLabels_[0] = "Stop Server";
        menuLabels_[1] = "Server Status";
    } else {
        menuLabels_[0] = "Start Server";
        menuLabels_[1] = "Server Status";
    }
}

void ServerMenuScreen::deinit() {
    // Nothing to clean up yet
}

void ServerMenuScreen::show() {
    visible_ = true;
    shouldExit_ = false;
    needsRedraw_ = true;
    
    footerHints_.setHints({});
    footerHints_.setFocus(false);
    
#ifdef ESP32
    LOG_D("SERVERMENU", "Screen shown");
#endif
}

void ServerMenuScreen::hide() {
    visible_ = false;
    // Stop server if still running (defensive — normal path stops in handleInput)
    if (serverRunning_) {
        ServerManager::getInstance().stop();
        serverRunning_ = false;
        adversary_ui_restore_canvas();
    }
}

void ServerMenuScreen::update() {
    // Check if server state changed externally
    bool isRunning = ServerManager::getInstance().isRunning();
    if (isRunning != serverRunning_) {
        serverRunning_ = isRunning;
        needsRedraw_ = true;
    }
}

void ServerMenuScreen::render(Canvas& canvas) {
#ifdef ESP32
    if (!needsRedraw_) return;
    needsRedraw_ = false;
    
    canvas.fillScreen(theme::BG_PRIMARY());
    
    drawHeader(canvas);
    drawMenu(canvas);
    drawFooter(canvas);
    
    // Render action menu on top if visible
    actionMenu_.render(canvas);
#else
    (void)canvas;
#endif
}

void ServerMenuScreen::drawHeader(Canvas& canvas) {
    ui::StatusBar::render(canvas, "SERVER MODE", nullptr, theme::BG_SECONDARY(), theme::TEXT_PRIMARY());
}

void ServerMenuScreen::drawMenu(Canvas& canvas) {
#ifdef ESP32
    int startY = HEADER_HEIGHT + 8;
    
    // Get device name for AP display
    auto& settings = SettingsManager::getInstance().get();
    
    // Show device name (will be AP SSID)
    canvas.setTextColor(theme::TEXT_SECONDARY());
    canvas.setTextSize(1);
    canvas.setCursor(8, startY);
    
    if (serverRunning_) {
        // Everything the operator needs to join: SSID + WPA2 key, and the fixed join URL.
        // This frame is painted once before the canvas is purged, so it shows static info
        // (the AP IP is always kDashboardIp); live counters can't update while purged.
        canvas.printf("SSID: %s", settings.system.deviceName);
        canvas.setCursor(canvas.width() - 72, startY);
        canvas.printf("%s", ServerManager::kDashboardIp);
        canvas.setCursor(8, startY + 11);
        canvas.printf("Key: %s", settings.system.dashboardApPassword);
        startY += 22;
    } else {
        canvas.printf("AP SSID: %s", settings.system.deviceName);
        startY += 16;
    }
    
    // The shared component paints the selected-row band and (when needed) the thumb; this list
    // never overflows, so no thumb draws and the payoff is the unified band idiom. The callback
    // draws each item's label plus its coloured status suffix (slice-0063/0065).
    menuList_.setItemCount(MENU_ITEMS);
    const int16_t listH = (int16_t)(MENU_ITEMS * theme::LIST_ROW_HEIGHT);
    ScrollableList::render(canvas, menuList_, 0, (int16_t)startY, canvas.width(), listH,
        theme::LIST_ROW_HEIGHT, [this, &settings](Canvas& c, int i, int16_t rowY, bool /*selected*/) {
            c.setTextSize(1);
            c.setTextColor(theme::TEXT_PRIMARY());
            c.setCursor(12, rowY + theme::LIST_ROW_TEXT_DY);
            if (i == 0) {
                c.print(serverRunning_ ? "Stop Server" : "Start Server");
            } else if (i == 1) {
                c.print("Server Status");
                if (serverRunning_) {
                    c.setTextColor(theme::SUCCESS());
                    c.print(" [RUNNING]");
                } else {
                    c.setTextColor(theme::TEXT_SECONDARY());
                    c.print(" [STOPPED]");
                }
            } else if (i == 2) {
                c.print("Configure Auth");
                if (settings.system.dashboardAuthEnabled) {
                    c.setTextColor(theme::SUCCESS());
                    c.print(" [ON]");
                } else {
                    c.setTextColor(theme::TEXT_SECONDARY());
                    c.print(" [OFF]");
                }
            }
        });
#else
    (void)canvas;
#endif
}

void ServerMenuScreen::drawFooter(Canvas& canvas) {
    footerHints_.render(canvas);
}

bool ServerMenuScreen::handleInput(char key) {
    needsRedraw_ = true;
    
    // Route to action menu if visible
    if (actionMenu_.isVisible()) {
        if (actionMenu_.handleInput(key)) {
            needsRedraw_ = true;
            return true;
        }
    }
    
    // Footer hints: focus navigation + action dispatch
    {
        char footerAction = 0;
        if (footerHints_.handleInputWithDispatch(key, footerAction)) {
            if (footerAction) return handleInput(footerAction);  // Dispatch action
            needsRedraw_ = true;
            return true;  // Consumed (navigation/toggle)
        }
    }
    
    switch (key) {
        case ';': // Up (wraps)
        case 'w':
            menuList_.setItemCount(MENU_ITEMS);
            menuList_.moveUp();
            return true;

        case '.': // Down (wraps)
        case 's':
            menuList_.setItemCount(MENU_ITEMS);
            menuList_.moveDown();
            return true;

        case '\r': // Enter
        case '\n':
            // Handle menu selection
            switch (menuList_.selectedIndex()) {
                case 0: // Start/Stop Server
                    if (serverRunning_) {
                        ServerManager::getInstance().stop();
                        serverRunning_ = false;
                        adversary_ui_restore_canvas();
                        ToastManager::getInstance().show("Server Stopped", ToastType::INFO);
                    } else {
                        // Provision the AP key and paint the running screen (SSID + key +
                        // join URL) to the LCD BEFORE purging the canvas: server mode frees
                        // the canvas for heap headroom, after which nothing can redraw, so
                        // the panel must already hold the connection info the operator needs
                        // to join (slice-0036). Heap order is unchanged — purge still
                        // precedes start().
                        if (!ServerManager::getInstance().ensureApKey()) {
                            ToastManager::getInstance().show("Key gen failed", ToastType::ERROR);
                            return true;
                        }
                        serverRunning_ = true;
                        adversary_ui_render_forced();   // draw + push the running frame
                        adversary_ui_purge_canvas();    // free RAM; LCD keeps the frame
                        if (!ServerManager::getInstance().start()) {
                            serverRunning_ = false;
                            adversary_ui_restore_canvas();
                            ToastManager::getInstance().show("Server Failed", ToastType::ERROR);
                        }
                    }
                    break;
                    
                case 1: // Server Status
                    if (!serverRunning_) {
                        ToastManager::getInstance().show("Server not running", ToastType::INFO);
                    } else {
                        ScreenManager::getInstance().setActiveScreen(ScreenId::SERVER_STATUS);
                    }
                    break;
                    
                case 2: // Configure Auth - go to settings
                    ToastManager::getInstance().show("Use Settings > Dashboard Server", ToastType::INFO);
                    break;
            }
            return true;
            
        case '`': // Back
            if (serverRunning_) {
                ServerManager::getInstance().stop();
                serverRunning_ = false;
                adversary_ui_restore_canvas();
            }
            shouldExit_ = true;
            return true;
    }
    
    return false;
}

} // namespace adversary
