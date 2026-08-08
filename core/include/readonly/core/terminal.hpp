#pragma once 
#include <string>
#include "readonly/core/error.hpp"
#include "readonly/core/vsock.hpp"
#include "readonly/shared/protocol.hpp"

namespace readonly::core {
using core::Result;
using core::VsockClient;
using shared::WinSize;

// Raw-mode host terminal. Restores termios on every exit path
//   RAII + signals + atexit
// Reserves bottom row for the gold READONLY bar and sizes
// the guest PTY to rows-1 so the agent never paints over it

class TerminalSession {
public: 
    static Result<TerminalSession> enter();
    ~TerminalSession();

    // Guest PTY size
    WinSize agent_winsize() const;   

    void draw_bar();              // paint gold bar on bottom (repeat)
    void clear_bar();             // release row before handing screen back

    // Drive session e2e over already run vsock
    // raw stdin -> DATA, DATA -> stdout, SIGWINCH -> WINSZ
    // , repaint bar, return guest cmd exit code. Blocks until then
    Result<int> pump(VsockClient& vs);

    TerminalSession(TerminalSession&&) noexcept;
    TerminalSession& operator=(TerminalSession&&) noexcept;
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;
private:
    TerminalSession() = default;
    bool active_{false};
};
} // namespace readonly::cli