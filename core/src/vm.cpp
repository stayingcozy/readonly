#include "readonly/core/vm.hpp"
#include "readonly/core/vsock.hpp"
#include "readonly/shared/protocol.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <format>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

extern char **environ;

namespace readonly::core {

using shared::kVsockPort;

// -- accel detection ---------

AccelInfo detect_accel() {
#if defined(__linux__)
  const int fd = ::open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (fd >= 0) {
    ::close(fd);
    return {Accel::Kvm, false};
  }
  if (errno == EACCES || errno == EPERM)
    return {Accel::None, true}; // in-group fix needed
  return {Accel::None, false};  // no kvm at all
#elif defined(__APPLE__)
  return {Accel::Hvf, false};
#elif defined(_WIN32)
  return {Accel::Whpx, false};
#else
  return {Accel::None, false};
#endif
}

namespace {
struct AccelFlags {
  std::string machine_accel;
  std::string cpu;
};

AccelFlags accel_flags(Accel a) {
  switch (a) {
  case Accel::Kvm:
    return {"kvm", "host"};
  case Accel::Hvf:
    return {"hvf", "host"};
  case Accel::Whpx:
    return {"whpx", "max"};
  case Accel::None:
    return {"tcg", "max"};
  }
  return {"tcg", "max"};
}
} // namespace

// -- argv assembly (pure) -----------
std::vector<std::string> Vm::build_argv(const QemuConfig &c) {
  const auto [maccel, cpu] = accel_flags(c.accel);

  std::vector<std::string> a{
      "qemu-system-x86_64",
      "-machine",
      std::format("q35,accel={}", maccel),
      "-cpu",
      cpu,
      "-m",
      std::to_string(c.memory_mb),
      "-kernel",
      c.kernel.string(),
      "-append",
      c.append, // one argv element — spaces need no quoting without a shell
      "-drive",
      std::format("file={},if=virtio,format=qcow2", c.image.string()),
      "-netdev",
      "user,id=n0",
      "-device",
      "virtio-net-pci,netdev=n0",
      "-fsdev",
      std::format("local,id=srcfs,path={},security_model=none,readonly=on",
                  c.src_share.string()),
      "-device",
      "virtio-9p-pci,fsdev=srcfs,mount_tag=src",
      "-fsdev",
      std::format("local,id=outfs,path={},security_model=none",
                  c.out_share.string()),
      "-device",
      "virtio-9p-pci,fsdev=outfs,mount_tag=out",
      "-device",
      std::format("vhost-vsock-pci,guest-cid={}", c.guest_cid),
  };

  if (c.serial_log) {
    a.insert(a.end(), {"-display", "none", "-serial",
                       std::format("file:{}", c.serial_log->string())});

  } else {
    a.emplace_back("-nographic");
  }
  return a;
}

// ---- launch / lifecycle ------------------------------------------------

namespace {
Result<void> must_be_file(const fs::path &p, std::string_view what) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec))
    return fail(std::format("{} not found: {}", what, p.string()));
  return {};
}
Result<void> must_be_dir(const fs::path &p, std::string_view what) {
  std::error_code ec;
  if (!fs::is_directory(p, ec))
    return fail(std::format("{} is not a directory: {}", what, p.string()));
  return {};
}
} // namespace

Result<Vm> Vm::launch(const QemuConfig &c) {
  if (auto r = must_be_file(c.image, "overlay image"); !r)
    return std::unexpected(r.error());
  if (auto r = must_be_file(c.kernel, "kernel bzImage"); !r)
    return std::unexpected(r.error());
  if (auto r = must_be_dir(c.src_share, "src share"); !r)
    return std::unexpected(r.error());
  if (auto r = must_be_dir(c.out_share, "out share"); !r)
    return std::unexpected(r.error());

  // vhost-vsock module must be loaded, or the -device line fails opaquely.
  if (::access("/dev/vhost-vsock", F_OK) != 0)
    return fail("/dev/vhost-vsock missing — run: sudo modprobe vhost_vsock");

  const auto argv_s = build_argv(c);
  std::vector<char *> argv;
  argv.reserve(argv_s.size() + 1);
  for (const auto &s : argv_s)
    argv.push_back(const_cast<char *>(s.c_str()));
  argv.push_back(nullptr);

  pid_t pid = -1;
  const int rc =
      ::posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
  if (rc != 0) {
    if (rc == ENOENT)
      return fail("qemu-system-x86_64 not found on PATH — install QEMU");
    return fail(std::format("cannot spawn qemu: {}", std::strerror(rc)));
  }

  // Catch instant death (bad args, missing device) so launch() reports it,
  // rather than the caller discovering it later at wait().
  ::timespec ts{0, 200'000'000}; // 200ms
  ::nanosleep(&ts, nullptr);
  int status = 0;
  const pid_t w = ::waitpid(pid, &status, WNOHANG);
  if (w == pid) {
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return fail(std::format("qemu exited immediately (code {}) — check the "
                            "argv (readonly __boot prints it)",
                            code));
  }

  Vm vm;
  vm.pid_ = pid;
  return vm;
}

Result<int> Vm::wait() {
  if (pid_ == -1)
    return fail("no VM process to wait on (already terminated)");
  int status = 0;
  while (::waitpid(pid_, &status, 0) < 0) {
    if (errno == EINTR)
      continue;
    const std::string why = std::strerror(errno);
    pid_ = -1;
    return fail(std::format("waitpid failed: {}", why));
  }
  pid_ = -1;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return -1;
}

void Vm::kill() {
  if (pid_ == -1)
    return;
  ::kill(pid_, SIGKILL);
  int status = 0;
  while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
  }
  pid_ = -1;
}

void Vm::soft_kill(unsigned guest_cid) {
  if (pid_ == -1)
    return;
  if (auto vs = VsockClient::connect(guest_cid, kVsockPort); vs) {
    (void)vs->send_run("sync; poweroff"); // guest tears down its own socket
  }
}

Result<int> Vm::shutdown(unsigned guest_cid) {
  soft_kill(guest_cid);

  constexpr int kTimeoutMs = 5000;
  constexpr int kStepMs = 100;
  for (int waited = 0; waited < kTimeoutMs; waited += kStepMs) {
    if (pid_ == -1 || (::kill(pid_, 0) != 0 && errno == ESRCH)) {
      return wait();
    }
    ::timespec ts{0, kStepMs * 1'000'000L};
    ::nanosleep(&ts, nullptr);
  }
  kill();
  return fail(std::format("guest did not shut down within {}ms. Force kill.",
                          kTimeoutMs));
}

Vm::~Vm() { kill(); } // never orphan a QEMU holding 9p mounts

Vm::Vm(Vm &&o) noexcept : pid_(o.pid_) { o.pid_ = -1; }

Vm &Vm::operator=(Vm &&o) noexcept {
  if (this != &o) {
    kill();
    pid_ = o.pid_;
    o.pid_ = -1;
  }
  return *this;
}

} // namespace readonly::core
