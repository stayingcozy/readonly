#pragma once 
#include "readonly/core/error.hpp"
#include "readonly/shared/protocol.hpp"

namespace readonly::cli {
using core::Result;
using shared::WinSize;

// Raw-mode host terminal. Bridges stdin<->vsock and draws the gold bars
// Restores termios in the destructor no matter how we exit
class TerminalSession {
public: 
    static Result<TerminalSession> enter();
    ~TerminalSession();

    WinSize window_size() const;   // physical rows/cols now
    void draw_bars();              // gold top/bottom READONLY bars
    // PTY given to the agent is sized rows-2 so it never paints our bars.

    TerminalSession(TerminalSession&&) noexcept;
    TerminalSession& operator=(TerminalSession&&) noexcept;
    TerminalSessions(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;
private:
    bool active_{false};
};
} // namespace readonly::cli