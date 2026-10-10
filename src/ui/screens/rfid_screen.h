/**
 * @file rfid_screen.h
 * @brief Screen for RFID tag interaction
 */

#pragma once

#include "ui/screens/screen_interface.h"
#include "ui/components/footer_hints.h"
#include "modules/rfid/rfid_manager.h"
#include "modules/rfid/rfid_audit.h"
#include "modules/rfid/rfid_dump.h"
#include "modules/rfid/rfid_writer.h"
#include "ui/components/text_input_popup.h"
#include <string>
#include <vector>

namespace adversary {

class RfidScreen : public IScreen {
public:
    RfidScreen();
    virtual ~RfidScreen();

    /// One-shot: the next show() opens straight into the write-restore flow (slice-0043).
    /// Set by the menu dispatcher when the "RFID Write" entry is chosen; cleared on consume.
    static void requestWriteMode() { s_writeModeRequested = true; }

    void show() override;
    void hide() override;
    void update() override;
    void render(Canvas& canvas) override;
    void requestRedraw() override { m_needsRedraw = true; }
    bool handleInput(char key) override;
    const char* getName() const override { return "RFID"; }
    ScreenId getId() const override { return ScreenId::RFID; }
    bool isVisible() const override { return m_active; }
    bool shouldExitToMenu() const override { return m_shouldExit; }
    void resetExitFlag() override { m_shouldExit = false; }

private:
    RFIDManager& m_manager;
    std::unique_ptr<RfidAudit> m_audit;
    
    bool m_active;
    bool m_shouldExit;
    bool m_needsRedraw;
    uint32_t m_lastUpdate;
    
    // UI Components
    ui::FooterHints footerHints_;
    TextInputPopup nameInput_;
    
    // State
    enum class State {
        IDLE,
        READING,
        RESULT,
        AUDIT_PROMPT,   // Pre-audit filename/options prompt
        AUDITING,
        AUDIT_SUMMARY,  // Post-audit summary view
        SECTOR_DETAIL,  // Detailed sector view with keys
        DETAILS,
        WRITE_PICK,     // Choose a .nfc dump to restore (slice-0043)
        WRITE_PROMPT,   // Show the write plan + UID opt-in, confirm
        WRITING,        // Restore in progress
        WRITE_SUMMARY   // Post-write result
    };
    State m_state;
    AuditProgressInfo m_lastAuditProgress;
    float m_auditProgress;
    int m_keysFound;
    
    // Audit summary navigation
    int8_t m_summaryScroll;      // Scroll position in summary list
    int8_t m_summarySelection;   // Selected sector in summary
    uint32_t m_auditStartMs;     // For elapsed time tracking
    
    // Details screen scrolling
    int8_t m_detailsScroll;      // Scroll position in details screen
    
    // Pre-audit options
    int8_t m_promptSelection;     // Selected option in audit prompt (0=readData, 1=start)
    bool m_readDataOption;        // Whether to read block data
    char m_auditFilename[64];     // Filename for audit results

    // Write/restore flow (slice-0043)
    static bool s_writeModeRequested;      // One-shot launch intent from the menu dispatcher
    std::vector<std::string> m_writeFiles; // .nfc dumps available under the RFID captures dir
    int8_t m_writeFileSelection;
    int8_t m_writeFileScroll;
    NfcDump m_writeDump;                    // Parsed dump for the chosen file
    WritePlan m_writePlan;                  // Blocks the restore will write
    WriteResult m_writeResult;              // Outcome, shown on WRITE_SUMMARY
    bool m_writeIncludeUid;                 // UID (block 0) opt-in toggle
    bool m_writeFromMenu;                   // true if entered via the RFID Write menu entry,
                                            // false if via the dashboard 'w' action — decides
                                            // where ESC/Done returns (submenu vs reader)
    int8_t m_writePromptSelection;          // 0 = UID toggle, 1 = start
    int m_writeProgressDone;                // Blocks processed so far (for the progress bar)
    int m_writeProgressTotal;

    // Drawing methods
    void drawDashboard(Canvas& canvas);
    void drawAuditProgress(Canvas& canvas);
    void drawAuditPrompt(Canvas& canvas);
    void drawAuditSummary(Canvas& canvas);
    void drawSectorDetail(Canvas& canvas);
    void drawDetailsSubscreen(Canvas& canvas);
    void drawWritePick(Canvas& canvas);
    void drawWritePrompt(Canvas& canvas);
    void drawWriting(Canvas& canvas);
    void drawWriteSummary(Canvas& canvas);
    
    // Helper methods
    void drawSectorListItem(Canvas& canvas, int16_t y, int sector, 
                            const MifareSectorData& data, bool selected);
    int getVisibleSummaryItems() const;
    int getDetailsContentHeight() const;
    
    // Actions
    void promptAudit();
    void startAuditWithOptions(const char* filename, bool readData);
    void startAudit();  // Legacy, now calls promptAudit
    void onAuditComplete();
    void saveDump(const char* name);

    // Write/restore actions (slice-0043)
    void enterWritePick();                      // Scan the RFID captures dir, show the picker
    bool loadWriteDump(const std::string& filename);  // Read + parse + plan; false on failure
    void startWrite();                          // Run the restore and go to the summary
};

} // namespace adversary
