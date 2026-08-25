#include "readonly/core/vsock.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <format>
#include <linux/vm_sockets.h>
#include <sys/socket.h>
#include <unistd.h>

namespace readonly::core {
using shared::FrameType;

namespace {
int recv_all(int fd, void *buf, std::size_t n) {
  auto *p = static_cast<std::uint8_t *>(buf);
  std::size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, p + got, n - got);
    if (r == 0)
      return 0;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    got += static_cast<std::size_t>(r);
  }
  return 1;
}
int send_all(int fd, const void *buf, std::size_t n) {
  const auto *p = static_cast<const std::uint8_t *>(buf);
  std::size_t put = 0;
  while (put < n) {
    ssize_t w = ::write(fd, p + put, n - put);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    put += static_cast<std::size_t>(w);
  }
  return 1;
}
Result<void> send_frame(int fd, FrameType type, const void *payload,
                        std::uint32_t len) {
  std::uint8_t hdr[5];
  hdr[0] = static_cast<std::uint8_t>(type);
  hdr[1] = std::uint8_t(len);
  hdr[2] = std::uint8_t(len >> 8);
  hdr[3] = std::uint8_t(len >> 16);
  hdr[4] = std::uint8_t(len >> 24);
  if (send_all(fd, hdr, 5) != 1)
    return fail("vsock write failed (header)");
  if (len && send_all(fd, payload, len) != 1)
    return fail("vsock write failed (payload)");
  return {};
}
} // namespace

Result<VsockClient> VsockClient::connect(unsigned cid, unsigned port) {
  constexpr int kAttempts = 75;
  for (int i = 0; i < kAttempts; ++i) {
    int fd = ::socket(AF_VSOCK, SOCK_STREAM, 0);
    if (fd < 0)
      return fail(
          std::format("AF_VSOCK socket failed: {}", std::strerror(errno)));

    sockaddr_vm addr{};
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = cid;
    addr.svm_port = port;

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0) {
      VsockClient c;
      c.fd_ = fd;
      return c;
    }
    ::close(fd);
    ::timespec ts{0, 200'000'000};
    ::nanosleep(&ts, nullptr);
  }
  return fail(std::format(
      "could not reach guest supervisor on cid {} port {} within 15s"
      "(supervisor not started? check the serial log)",
      cid, port));
}

Result<void> VsockClient::send_run(std::string_view cmd) {
  return send_frame(fd_, FrameType::Run, cmd.data(),
                    static_cast<std::uint32_t>(cmd.size()));
}

Result<void> VsockClient::send_winsize(WinSize ws) {
  std::uint8_t p[4] = {std::uint8_t(ws.rows), std::uint8_t(ws.rows >> 8),
                       std::uint8_t(ws.cols), std::uint8_t(ws.cols >> 8)};
  return send_frame(fd_, FrameType::Winsz, p, 4);
}

Result<void> VsockClient::send_stdin(std::span<const std::byte> bytes) {
  return send_frame(fd_, FrameType::Data, bytes.data(),
                    static_cast<std::uint32_t>(bytes.size()));
}

Result<VsockClient::Frame> VsockClient::next_frame() {
  std::uint8_t hdr[5];
  int r = recv_all(fd_, hdr, 5);
  if (r == 0)
    return fail("supervisor closed the connection");
  if (r < 0)
    return fail(std::format("vsock read failed: {}", std::strerror(errno)));

  Frame f;
  f.type = static_cast<FrameType>(hdr[0]);
  const std::uint32_t len =
      std::uint32_t(hdr[1]) | (std::uint32_t(hdr[2]) << 8) |
      (std::uint32_t(hdr[3]) << 16) | (std::uint32_t(hdr[4]) << 24);

  std::string payload;
  payload.resize(len);
  if (len && recv_all(fd_, payload.data(), len) <= 0)
    return fail("vsock read failed (payload)");

  if (f.type == FrameType::Exit) {
    if (len >= 4) {
      f.exit_code = int(std::uint8_t(payload[0])) |
                    (int(std::uint8_t(payload[1])) << 8) |
                    (int(std::uint8_t(payload[2])) << 16) |
                    (int(std::uint8_t(payload[3])) << 24);
    }
  } else {
    f.data = std::move(payload);
  }
  return f;
}

VsockClient::~VsockClient() {
  if (fd_ != -1)
    ::close(fd_);
}
VsockClient::VsockClient(VsockClient &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
VsockClient &VsockClient::operator=(VsockClient &&o) noexcept {
  if (this != &o) {
    if (fd_ != -1)
      ::close(fd_);
    fd_ = o.fd_;
    o.fd_ = -1;
  }
  return *this;
}

} // namespace readonly::core