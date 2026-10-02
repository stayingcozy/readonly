#include "readonly/core/guest.hpp"
#include "readonly/core/vsock.hpp"
#include "readonly/shared/protocol.hpp"

#include <array>
#include <cerrno>
#include <optional>
#include <poll.h>
#include <span>
#include <unistd.h>

namespace readonly::core {
namespace shared = readonly::shared;

Result<int> exec_in_guest(const QemuConfig &cfg, std::string_view cmd,
                          std::string_view stdin_data, const GuestOutFn &on_out,
                          int stdin_fd,
                          std::chrono::milliseconds idle_timeout) {

  auto vm = Vm::launch(cfg);
  if (!vm)
    return std::unexpected(vm.error());

  std::optional<VsockClient> vs;
  if (auto c = VsockClient::connect(cfg.guest_cid, shared::kVsockPort); c) {
    vs.emplace(std::move(*c));
  } else {
    vm->kill();
    return std::unexpected(c.error());
  }

  auto bail = [&](Error e) -> Result<int> {
    vs.reset();
    (void)vm->shutdown(cfg.guest_cid);
    return std::unexpected(std::move(e));
  };

  if (auto r = vs->send_run(cmd); !r)
    return bail(r.error());

  if (!stdin_data.empty()) {
    auto bytes = std::as_bytes(std::span{stdin_data.data(), stdin_data.size()});
    if (auto r = vs->send_stdin(bytes); !r)
      return bail(r.error());
  }

  const bool has_live_stdin = stdin_fd >= 0;
  pollfd fds[2];
  fds[0] = {vs->fd(), POLLIN, 0};
  fds[1] = {has_live_stdin ? stdin_fd : -1, POLLIN, 0};
  const nfds_t nfds = has_live_stdin ? 2 : 1;

  const int timeout_ms = static_cast<int>(idle_timeout.count());
  int exit_code = -1;
  bool got_exit = false;
  std::optional<Error> stream_err;

  for (;;) {
    int pr = ::poll(fds, nfds, timeout_ms);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      stream_err = Error{"poll() failed on vsock fd"};
      break;
    }
    if (pr == 0) {
      stream_err = Error{"guest produced no frame within idle timeout"};
      break;
    }

    if (has_live_stdin && (fds[1].revents & POLLIN)) {
      std::array<std::byte, 4096> b;
      ssize_t n = ::read(stdin_fd, b.data(), b.size());
      if (n > 0)
        (void)vs->send_stdin({b.data(), static_cast<std::size_t>(n)});
    };

    if (fds[0].revents & (POLLIN | POLLHUP)) {
      auto f = vs->next_frame();
      if (!f) {
        stream_err = f.error();
        break;
      } // supervisor closed connection
      if (f->type == shared::FrameType::Data) {
        if (on_out)
          on_out(f->data);
      } else if (f->type == shared::FrameType::Exit) {
        exit_code = f->exit_code;
        got_exit = true;
        break;
      }
    }
  }

  vs.reset();
  (void)vm->shutdown(cfg.guest_cid);

  if (!got_exit)
    return std::unexpected(
        stream_err.value_or(Error{"stream ended before Exit"}));
  return exit_code;
}

Result<GuestRun> run_in_guest(const QemuConfig &cfg, std::string_view cmd,
                              std::string_view stdin_data,
                              std::chrono::milliseconds idle_timeout) {
  GuestRun run;
  auto ec = exec_in_guest(
      cfg, cmd, stdin_data,
      [&](std::string_view chunk) { run.output += chunk; }, /*stdin_fd=*/-1,
      idle_timeout);
  if (!ec)
    return std::unexpected(ec.error());
  run.exit_code = *ec;
  return run;
}
} // namespace readonly::core
