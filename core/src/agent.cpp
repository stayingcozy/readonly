#include "readonly/core/agent.hpp"
#include "readonly/core/snapshot.hpp"
#include "readonly/core/terminal.hpp"
#include "readonly/core/vm.hpp"
#include "readonly/core/vsock.hpp"
#include "readonly/shared/protocol.hpp"

#include <csignal>
#include <format>
#include <iostream>
#include <optional>
#include <print>
#include <random>
#include <string>
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
Result<int> interactive_session(std::string_view command) {
  auto vs = VsockClient::connect(3 /*guest_cid*/, shared::kVsockPort);
  if (!vs)
    return std::unexpected(vs.error());
  if (auto r = vs->send_run(command); !r)
    return std::unexpected(r.error());

  auto term = TerminalSession::enter();
  if (!term)
    return std::unexpected(term.error());
  return term->pump(*vs); // restores terminal in its own destructor
}

// RAII: ignore SIGINT for install/reauth
class SigintIgnored {
public:
  SigintIgnored() {
    struct sigaction ign{};
    ign.sa_handler = SIG_IGN;
    ::sigaction(SIGINT, &ign, &old_);
  }
  ~SigintIgnored() { ::sigaction(SIGINT, &old_, nullptr); }
  SigintIgnored(const SigintIgnored &) = delete;
  SigintIgnored &operator=(const SigintIgnored &) = delete;

private:
  struct sigaction old_{};
};

// Known vendor from install cmd; shared by infer_name + auth_probe
std::optional<std::string_view> known_kind(std::string_view cmd) {
  struct Known {
    std::string_view needle, name;
  };
  constexpr Known table[] = {
      {"anthropic", "claude"}, {"claude", "claude"},   {"codex", "codex"},
      {"chatgpt", "codex"},    {"copilot", "copilot"}, {"gh.io", "copilot"},
  };
  for (const auto &k : table)
    if (cmd.find(k.needle) != std::string_view::npos)
      return k.name;
  return std::nullopt;
}

// Guest shell test, exit 0 iff creds stored. Keyed on install_cmd (not name)
// so --name overrides still probe. nullopt -> can't verify, ask user
std::optional<std::string_view> auth_probe(std::string_view install_cmd) {
  const auto kind = known_kind(install_cmd);
  if (kind == "claude")
    return R"(test -s "$HOME/.claude/.credentials.json")";
  if (kind == "codex")
    return R"(test -s "$HOME/.codex/auth.json")";
  return std::nullopt; // copilot: creds location unverified
}

// Non-interactive cmd on the already-booted VM; output discarded
Result<int> quiet_session(std::string_view command) {
  auto vs = VsockClient::connect(3, shared::kVsockPort);
  if (!vs)
    return std::unexpected(vs.error());
  if (auto r = vs->send_run(command); !r)
    return std::unexpected(r.error());
  for (;;) {
    auto f = vs->next_frame();
    if (!f)
      return std::unexpected(f.error());
    if (f->type == shared::FrameType::Exit)
      return f->exit_code;
  }
}

enum class AuthOutcome { Authed, Unverified, Abort };

// Run agent interactively until probe passes or the user decides
Result<AuthOutcome> auth_loop(std::string_view name, std::string_view run_cmd,
                              std::optional<std::string_view> probe) {
  for (;;) {
    // agent exit code irrelevant: user auths then dips
    if (auto code = interactive_session(run_cmd); !code)
      return std::unexpected(code.error());

    if (probe) {
      auto ok = quiet_session(*probe);
      if (!ok)
        return std::unexpected(ok.error());
      if (*ok == 0)
        return AuthOutcome::Authed;
      std::println(stderr, "\nno saved login found for '{}'.", name);
    } else {
      std::println(stderr, "\ncannot verify login for '{}'.", name);
    }

    std::print(stderr, "[r]etry login, [s]ave anyway, [a]bort? ");
    std::string ans;
    if (!std::getline(std::cin, ans) || ans.starts_with('a'))
      return AuthOutcome::Abort;
    if (ans.starts_with('s')) {
      std::println(stderr, "saving; run `readonly reauth {}` to fix later.",
                   name);
      return AuthOutcome::Unverified;
    }
  }
}

// Clean poweroff, then atomic rename. Never commit after a forced kill:
// an unflushed qcow2 can be torn
Result<void> commit_overlay(Vm &vm, OverlayGuard &staging,
                            const fs::path &final_path) {
  if (auto r = vm.shutdown(3); !r)
    return fail(std::format("guest did not power off cleanly, not saved: {}",
                            r.error().message));
  std::error_code ec;
  fs::rename(staging.path(), final_path, ec); // replaces atomically
  if (ec)
    return fail(std::format("cannot commit overlay to {}: {}",
                            final_path.string(), ec.message()));
  staging.keep();
  return {};
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

  // 4. run agent's command interactively from the mounted source (supervisor
  // shell starts in /); VM hard-killed after
  const auto probe = auth_probe(desc->install_cmd);
  const bool had_login = !probe || quiet_session(*probe).value_or(1) == 0;
  const std::string cmd = std::format("cd {} || exit 1; {}",
                                      shared::kGuestSrcDir, desc->run_cmd);
  auto code = interactive_session(cmd);
  vm->kill();
  if (!had_login)
    std::println(stderr,
                 "note: '{}' has no saved login, and logins made in a run are "
                 "discarded. Run `readonly reauth {}` to save one.",
                 name, name);
  // scratch + overlay_guard destructor discard everythign here
  return code; // exit code || session error
}

// --- install ---

Result<void> AgentManager::install(std::string_view install_cmd,
                                   std::optional<std::string> name_opt) {
  SigintIgnored sigint_ignored;

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

  const std::string run_cmd =
      std::format("export PATH=\"$HOME/.local/bin:$PATH\"; {}", name);

  // Session 2..n: auth until verified, or user saves/aborts
  // (each session reconnects: supervisor loops back to accept())
  auto outcome = auth_loop(name, run_cmd, auth_probe(install_cmd));
  if (!outcome) {
    vm->kill();
    return std::unexpected(outcome.error());
  }
  if (*outcome == AuthOutcome::Abort) {
    vm->kill();
    return fail("install aborted; nothing saved");
  }

  // Clean flush + poweroff, then commit atomically
  if (auto c = commit_overlay(*vm, guard, registry_.overlay_path(name)); !c)
    return c;

  AgentDescriptor d;
  d.name = name;
  d.surface = shared::Surface::Cli; // v1 CLI only
  d.run_cmd = run_cmd;
  d.install_cmd = std::string{install_cmd};
  if (auto w = registry_.write(d); !w) {
    // Overlay commited but meta failed: resolve() tolerates missing meta,
    // agent works with defaults. Surface warning
    return std::unexpected(w.error());
  }
  return {};
}

// --- reauth ---

Result<void> AgentManager::reauth(std::string_view name) {
  SigintIgnored sigint_ignored;

  auto deps = resolve_deps();
  if (!deps)
    return std::unexpected(deps.error());
  auto desc = registry_.resolve(name);
  if (!desc)
    return std::unexpected(desc.error());

  auto scratch = RunScratch::create(paths_);
  if (!scratch)
    return std::unexpected(scratch.error());

  // Work on a copy: aborted auth or a live `readonly <name>` (backed by
  // final_path) never sees a half-written snapshot
  const fs::path final_path = registry_.overlay_path(name);
  const fs::path staging = scratch->run_dir() / "reauth.qcow2";
  std::error_code ec;
  fs::copy_file(final_path, staging, ec);
  if (ec)
    return fail(std::format("cannot stage {}: {}", final_path.string(),
                            ec.message()));
  OverlayGuard guard(staging); // rm unless committed

  auto vm = Vm::launch(
      make_config(staging, deps->kernel, scratch->src(), scratch->out()));
  if (!vm)
    return std::unexpected(vm.error());

  auto outcome = auth_loop(name, desc->run_cmd, auth_probe(desc->install_cmd));
  if (!outcome) {
    vm->kill();
    return std::unexpected(outcome.error());
  }
  if (*outcome == AuthOutcome::Abort) {
    vm->kill();
    return fail("reauth aborted; snapshot unchanged");
  }
  return commit_overlay(*vm, guard, final_path);
}

// --- name inference ---

std::string AgentManager::infer_name(std::string_view cmd) {
  if (auto k = known_kind(cmd))
    return std::string{*k};

  // Fallback: random readonly-safe name
  std::random_device rd;
  return std::format("agent-{:08x}", rd());
}

} // namespace readonly::core
