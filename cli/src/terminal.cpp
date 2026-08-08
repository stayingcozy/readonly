#include "readonly/cli/terminal.hpp"

#include <atomic>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace readonly::cli {
using shared::FrameType;

// -- global restore state --
// termios restore w/ signal handler & atexit -> file-scope atomics
namespace {
termios            g_orig{};
std::atomic<bool>  g_raw_active{false};
volatile std::sig_atomic_t g_winch = 0;

constexpr std::string_view kGold    = "\033[48;5;220m\033[38;5;16m"; // gold bg, near-black fg
constexpr std::string_view kReset   = "\033[0m";
constexpr std::string_view kSaveCur = "\033[s";
constexpr std::string_view kRestCur = "\033[u";

void restore_termios() {
    if (g_raw_active.exchange(false))
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
}

void on_fatal_signal(int sig) {
    restore_termios();
    ::signal(sig, SIG_DFL);
    ::raise(sig);             // die as if unhandled, but with sane terminal
}

void on_winch(int) { g_winch = 1; }

WinSize physical_size() {
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0)
        return {24, 80}; 
    return {ws.ws_row, ws.ws_col};
}
} // namespace

// --- lifecycle ---
Result<TerminalSession> TerminalSession::enter() {
    if (!::isatty(STDIN_FILENO))
        return core::fail("stdin is not a terminal (readonly needs an interactive tty)");

    if(::tcgetattr(STDIN_FILENO, &g_orig) != 0)
        return core::fail(std::format("tcgetattr failed: {}", std::strerror(errno)));

    termios raw = g_orig;
    ::cfmakeraw(&raw);              // no echo/canonical/signal_chars
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) 
        return core::fail(std::format("tcsetattr failed: {}", std::strerror(errno)));

    g_raw_active.store(true);
    std::atexit(restore_termios);    // final backstop

    struct sigaction sa{};
    sa.sa_handler = on_fatal_signal;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP,  &sa, nullptr);
    // NOT SIGINT -> Ctrl+C is byte fwd to agent (pump())

    struct sigaction sw{};
    sw.sa_handler = on_winch;
    ::sigaction(SIGWINCH, &sw, nullptr);

    TerminalSession s;
    s.active_ = true;
    return s;
}

TerminalSession::~TerminalSession() {
    if (active_) { clear_bar(); restore_termios(); }
}

TerminalSession::TerminalSession(TerminalSession&& o) noexcept : active_(o.active_) {
    o.active_ = false;
}

TerminalSession& TerminalSession::operator=(TerminalSession&& o) noexcept {
    if (this != &o) {
        if (active_) { clear_bar(); restore_termios(); }
        active_ = o.active_;
        o.active_ = false;
    }
    return *this;
}

// --- geometry + bar ---

WinSize TerminalSession::agent_winsize() const {
    WinSize p = physical_size();
    return { static_cast<std::uint16_t>(p.rows > 1 ? p.rows - 1 : 1), p.cols };
}

void TerminalSession::draw_bar() {
    const WinSize p = physical_size();
    const int bar_row = p.rows; 

    std::string label = " READONLY ";
    std::string bar;
    bar += kSaveCur;
    bar += std::format("\033[{};1H", bar_row);
    bar += kGold;
    bar += label; 
    // file rest of row so gold spans full width
    for (int c = static_cast<int>(label.size()); c < p.cols; ++c) bar += ' ';
    bar += kReset; 
    bar += kRestCur; 
    ::write(STDOUT_FILENO, bar.data(), bar.size());
}

void TerminalSession::clear_bar() {
    const WinSize p = physical_size();
    std::string s;
    s += kSaveCur; 
    s += std::format("\033[{};1H\033[2K", p.rows); // clear bot row
    ::write(STDOUT_FILENO, s.data(), s.size());
}

// --- pump ---

Result<int> TerminalSession::pump(VsockClient& vs) {
    // agent's view of screen out the gate
    if (auto r = vs.send_winsize(agent_winsize()); !r) return std::unexpected(r.error());
    draw_bar();

    pollfd fds[2];
    fds[0] = { STDIN_FILENO, POLLIN, 0 };
    fds[1] = { vs.fd(),      POLLIN, 0 };

    for (;;) {
        const int pr = ::poll(fds, 2, 250); // 250ms wakeup kees bar alive
        if (pr < 0) {
            if (errno == EINTR) {
                if (g_winch) {
                    g_winch = 0;
                    if (auto r = vs.send_winsize(agent_winsize()); !r)
                        return std::unexpected(r.error());
                    draw_bar();
                }
                continue;
            }
            return core::fail(std::format("poll failed: {}", std::strerror(errno)));
        }

        if (g_winch) {
            g_winch = 0;
            if (auto r = vs.send_winsize(agent_winsize()); !r)
                return std::unexpected(r.error());
            draw_bar();
        }

        if (pr == 0) { draw_bar(); continue; } // idle tick

        if (fds[0].revents & POLLIN) { // host stdin -> guest (raw bytes)
            std::array<std::byte, 4096> b;
            const ssize_t n = ::read(STDIN_FILENO, b.data(), b.size());
            if (n > 0) {
                if (auto r = vs.send_stdin({b.data(), static_cast<std::size_t>(n)}); !r)
                    return std::unexpected(r.error());
            }
        }

        if (fds[1].revents & (POLLIN | POLLHUP)) { // guest -> host stdout
            auto f = vs.next_frame();
            if (!f) return std::unexpected(f.error());
            if (f->type == FrameType::Data) {
                ::write(STDOUT_FILENO, f->data.data(), f->data.size());
                draw_bar();                        // agent just painted; reassert dominance
            } else if (f->type == FrameType::Exit) {
                clear_bar();
                return f->exit_code;
            }
        }
    }
}

} // readonly::cli