#include "readonly/core/agent.hpp"
#include "readonly/core/snapshot.hpp"
#include "readonly/core/terminal.hpp"
#include "readonly/core/vm.hpp"
#include "readonly/core/vsock.hpp"
#include "readonly/shared/protocol.hpp"

#include <format>
#include <random>
#include <system_error>

namespace readonly::core {

namespace {
// RAII: remove overlay unless explicitly kept; run overlay discarded on every
// path
class OverlayGuard {
public:
  explicit OverlayGuard(fs::path p) : path_(std::move(p)) {}
  ~OverlayGuard() {
    if (armed_) {
      std::error_code ec;
      fs::remove(path_, ec);
    }
  }
  void keep() { armed_ = false; }
  const fs::path &path() const { return path_; }
  OverlayGuard(const OverlayGuard &) = delete;
  OverlayGuard &operator=(const OverlayGuard &) = delete;

private:
  fs::path path_;
  bool armed_{true};
};

QemuConfig make_config(const fs::path &image, const fs::path &kernel,
                       const fs::path &src, const fs::path &out) {
  QemuConfig c;
  c.image = image;
  c.kernel = kernel;
  c.src_share = src;
  c.out_share = out;
  c.accel = detect_accel().accel;
  c.serial_log = fs::absolute("readonly-serial.log"); // stdio is the UI
  return c;
}

// Boot existing config, connect, drive interactive cmd
// to complete via terminal
Result<int> interactive_session(Vm &vm, std::string_view command) {
  auto vs = VsockClient::connect(3 /*guest_cid*/, shared::kVsockPort);
  if (!vs)
    return std::unexpected(vs.error());
  if (auto r = vs->send_run(command); !r)
    return std::unexpected(r.error());

  auto term = TerminalSession::enter();
  if (!term)
    return std::unexpected(term.error());
  return term->pump(*vs); // restores terminal in its own destructor
  (void)vm;
}
} // namespace

AgentManager::AgentManager(Paths paths, Registry registry)
    : paths_(std::move(paths)), registry_(std::move(registry)) {}

Result<AgentManager::Deps> AgentManager::resolve_deps() const {
  Deps d;
  d.base_image = paths_.base_image();
  d.kernel = paths_.root() / "bzImage";
  std::error_code ec;
  if (!fs::is_regular_file(d.base_image, ec))
    return fail(std::format("base image missing: {} (run `readonly setup`)",
                            d.base_image.string()));
  if (!fs::is_regular_file(d.kernel, ec))
    return fail(std::format("kernel missing: {} (run `readonly setup`)",
                            d.kernel.string()));
  return d;
}

// --- run ---

Result<int> AgentManager::run(std::string_view name, const fs::path &target,
                              const MaskSpec &mask) {
  auto deps = resolve_deps();
  if (!deps)
    return std::unexpected(deps.error());

  auto desc = registry_.resolve(name);
  if (!desc)
    return std::unexpected(desc.error());

  // 1. Mirror Source (masked hardlink) + create writable out dir
  auto scratch = RunScratch::create(paths_);
  if (!scratch)
    return std::unexpected(scratch.error());
  auto stats = scratch->mirror_source(target, mask);
  if (!stats)
    return std::unexpected(stats.error());

  // 2. Throwaway overlay on agent snapshot - base + agent stay pristine
  const fs::path run_overlay = scratch->run_dir() / "run.qcow2";
  if (auto r =
          Snapshot::create_overlay(registry_.overlay_path(name), run_overlay);
      !r)
    return std::unexpected(r.error());
  OverlayGuard overlay_guard(run_overlay); // discarded on every exit path

  // 3. boot
  QemuConfig cfg =
      make_config(run_overlay, deps->kernel, scratch->src(), scratch->out());
  auto vm = Vm::launch(cfg);
  if (!vm)
    return std::unexpected(vm.error());

  // 4. run agent's command interactively; VM hard-killed after
  auto code = interactive_session(*vm, desc->run_cmd);
  vm->kill();
  // scratch + overlay_guard destructor discard everythign here
  return code; // exit code || session error
}

// --- install ---

Result<void> AgentManager::install(std::string_view install_cmd,
                                   std::optional<std::string> name_opt) {
  auto deps = resolve_deps();
  if (!deps)
    return std::unexpected(deps.error());

  const std::string name = name_opt.value_or(infer_name(install_cmd));
  if (auto v = Registry::validate_name(name); !v)
    return std::unexpected(v.error());
  if (registry_.exists(name))
    return fail(
        std::format("agent '{}' already installed (uninstall it first)", name));

  // Build temp overlay; rename agents/ after clean shutdown
  auto scratch = RunScratch::create(paths_);
  if (!scratch)
    return std::unexpected(scratch.error());
  const fs::path staging = scratch->run_dir() / "install.qcow2";
  if (auto r = Snapshot::create_overlay(deps->base_image, staging); !r)
    return std::unexpected(r.error());
  OverlayGuard guard(staging); // rm unless succeed

  // Boot base with staging overlay. Needs network
  QemuConfig cfg =
      make_config(staging, deps->kernel, scratch->src(), scratch->out());
  auto vm = Vm::launch(cfg);
  if (!vm)
    return std::unexpected(vm.error());

  auto vs = VsockClient::connect(3, shared::kVsockPort);
  if (!vs) {
    vm->kill();
    return std::unexpected(vs.error());
  }

  // Session 1: run the install cmd interactively
  {
    if (auto r = vs->send_run(install_cmd); !r) {
      vm->kill();
      return std::unexpected(r.error());
    }
    auto term = TerminalSession::enter();
    if (!term) {
      vm->kill();
      return std::unexpected(term.error());
    }
    auto code = term->pump(*vs);
    if (!code) {
      vm->kill();
      return std::unexpected(code.error());
    }
    if (*code != 0) {
      vm->kill();
      return fail(std::format("install command exit {}", *code));
    }
  }

  // Session 2: run agent for user auth (device flow in host browser)
  // reconnect: supervisor looped back to accept() after sess 1
  {
    auto vs2 = VsockClient::connect(3, shared::kVsockPort);
    if (!vs2) {
      vm->kill();
      return std::unexpected(vs2.error());
    }
    if (auto r = vs2->send_run(name); !r) {
      vm->kill();
      return std::unexpected(r.error());
    }
    auto term = TerminalSession::enter();
    if (!term) {
      vm->kill();
      return std::unexpected(term.error());
    }
    (void)term->pump(*vs2); // exit code irrelevant; user need auth then dip
  }

  // Session 3: clean flush + poweroff, WAIT for qemu to exit before keeping
  // overlay
  {
    auto vs3 = VsockClient::connect(3, shared::kVsockPort);
    if (!vs3) {
      vm->kill();
      return std::unexpected(vs3.error());
    }
    (void)vs3->send_run(
        "sync; poweroff"); // fire and forget; guest tears down socket
  }
  auto exit_code = vm->wait(); // blocks until guest has flushed and powered off
  if (!exit_code) {
    vm->kill();
    return std::unexpected(exit_code.error());
  }

  // Clean shutdown verified, commit atomically
  const fs::path final_path = registry_.overlay_path(name);
  std::error_code ec;
  fs::rename(staging, final_path, ec);
  if (ec)
    return fail(std::format("cannot commit overlay to {}: {}",
                            final_path.string(), ec.message()));
  guard.keep(); // staging now final_path; DONT THINK ABOUT removing it

  AgentDescriptor d;
  d.name = name;
  d.surface = shared::Surface::Cli; // v1 CLI only
  d.run_cmd = name;
  d.install_cmd = std::string{install_cmd};
  if (auto w = registry_.write(d); !w) {
    // Overlay commited but meta failed: resolve() tolerates missing meta,
    // agent works with defaults. Surface warning
    return std::unexpected(w.error());
  }
  return {};
}

// --- name inference ---

std::string AgentManager::infer_name(std::string_view cmd) {
  struct Known {
    std::string_view needle, name;
  };
  constexpr Known table[] = {
      {"anthropic", "claude"}, {"claude", "claude"},   {"codex", "codex"},
      {"chatgpt", "codex"},    {"copilot", "copilot"}, {"gh.io", "copilot"},
  };
  for (const auto &k : table) {
    if (cmd.find(k.needle) != std::string_view::npos) {
      return std::string{k.name};
    }
  }

  // Fallback: random readonly-safe name
  std::random_device rd;
  return std::format("agent-{:08x}", rd());
}

} // namespace readonly::core