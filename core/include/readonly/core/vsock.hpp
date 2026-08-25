#pragma once
#include "readonly/core/error.hpp"
#include "readonly/shared/protocol.hpp"
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace readonly::core {
using shared::WinSize;

class VsockClient {
public:
  static Result<VsockClient> connect(unsigned cid, unsigned port);

  Result<void> send_run(std::string_view command);
  Result<void> send_winsize(WinSize);
  Result<void> send_stdin(std::span<const std::byte>);

  // One decoded inbound frame; the terminal loop drives this
  struct Frame {
    shared::FrameType type;
    std::string data;
    int exit_code{0};
  };
  Result<Frame> next_frame();

  int fd() const { return fd_; } // poll() in terminal loop
  ~VsockClient();

  VsockClient(VsockClient &&) noexcept;
  VsockClient &operator=(VsockClient &&) noexcept;
  VsockClient(const VsockClient &) = delete;
  VsockClient &operator=(const VsockClient &) = delete;

private:
  VsockClient() = default;
  int fd_{-1};
};
} // namespace readonly::core