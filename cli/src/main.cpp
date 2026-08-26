#include <CLI/CLI.hpp>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <poll.h>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

#include "readonly/core/agent.hpp"
#include "readonly/core/config.hpp"
#include "readonly/core/mounts.hpp"
#include "readonly/core/registry.hpp"
#include "readonly/core/snapshot.hpp"
#include "readonly/core/terminal.hpp"
#include "readonly/core/vm.hpp"
#include "readonly/core/vsock.hpp"
#include "readonly/shared/protocol.hpp"

namespace fs = std::filesystem;

using readonly::core::Accel;
using readonly::core::AgentManager;
using readonly::core::detect_accel;
using readonly::core::GlobalConfig;
using readonly::core::MaskSpec;
using readonly::core::Paths;
using readonly::core::QemuConfig;
using readonly::core::Registry;
using readonly::core::RunScratch;
using readonly::core::Snapshot;
using readonly::core::Vm;
using readonly::core::VsockClient;

namespace shared = readonly::shared;

namespace {

// Parse a comma-separated --mask string into a MaskSpec, defaulting to ".env".
MaskSpec parse_mask(const std::string &csv) {
  MaskSpec m;
  m.names.clear();
  std::string cur;
  for (char c : csv + ",") {
    if (c == ',') {
      if (!cur.empty())
        m.names.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (m.names.empty())
    m.names.push_back(".env");
  return m;
}

// `readonly <name> [path] [--mask ...]`
int run_agent_form(int argc, char **argv) {
  CLI::App app{"run an installed agent"};
  std::string name;
  std::string path{"."};
  std::string mask{".env"};
  app.add_option("name", name, "installed agent name")->required();
  app.add_option("path", path, "directory to expose read-only (default: cwd)");
  app.add_option("--mask", mask,
                 "comma-separated names to exclude from the mirror");
  CLI11_PARSE(app, argc, argv);

  auto paths = Paths::discover();
  if (!paths) {
    std::println(stderr, "error: {}", paths.error().message);
    return 1;
  }
  Registry reg{*paths};
  AgentManager mgr{*paths, reg};

  auto code = mgr.run(name, fs::absolute(path), parse_mask(mask));
  if (!code) {
    std::println(stderr, "error: {}", code.error().message);
    return 1;
  }
  return *code; // propagate the guest command's exit code
}

// hidden debug commands (delete this whole function once `setup` exists)
void add_debug_commands(CLI::App &app) {
  // __config — resolve paths + load global config
  app.add_subcommand("__config", "debug: resolved paths + config")
      ->group("")
      ->callback([] {
        auto paths = Paths::discover();
        if (!paths) {
          std::println(stderr, "error: {}", paths.error().message);
          return;
        }
        std::println("root:    {}", paths->root().string());
        std::println("agents:  {}", paths->agents_dir().string());
        std::println("scratch: {}", paths->scratch_root().string());
        std::println("base:    {}", paths->base_image().string());
        auto cfg = GlobalConfig::load(*paths);
        if (!cfg) {
          std::println(stderr, "config error: {}", cfg.error().message);
          return;
        }
        std::string masks;
        for (std::size_t i = 0; i < cfg->default_mask.size(); ++i) {
          if (i)
            masks += ',';
          masks += cfg->default_mask[i];
        }
        std::println("mask:    {}", masks);
        std::println("accel:   {}", cfg->accel_override.empty()
                                        ? "(auto)"
                                        : cfg->accel_override);
      });

  // __accel — acceleration detection
  app.add_subcommand("__accel", "debug: accel detection")
      ->group("")
      ->callback([] {
        auto info = detect_accel();
        const char *name = info.accel == Accel::Kvm    ? "kvm"
                           : info.accel == Accel::Hvf  ? "hvf"
                           : info.accel == Accel::Whpx ? "whpx"
                                                       : "none (tcg)";
        std::println("accel: {}", name);
        if (info.kvm_present_but_denied)
          std::println(
              "note: /dev/kvm exists but is not accessible — "
              "run: sudo usermod -aG kvm $USER, then log out and back in");
      });

  // __mirror — masked hardlink mirror, left on disk to inspect
  {
    static std::string path, mask{".env"};
    auto *c = app.add_subcommand("__mirror", "debug: masked hardlink mirror")
                  ->group("");
    c->add_option("path", path)->required();
    c->add_option("--mask", mask);
    c->callback([] {
      auto paths = Paths::discover();
      if (!paths) {
        std::println(stderr, "error: {}", paths.error().message);
        return;
      }
      auto scratch = RunScratch::create(*paths);
      if (!scratch) {
        std::println(stderr, "error: {}", scratch.error().message);
        return;
      }
      auto stats = scratch->mirror_source(path, parse_mask(mask));
      if (!stats) {
        std::println(stderr, "error: {}", stats.error().message);
        return;
      }
      std::println("mirror: {}", scratch->src().string());
      std::println("linked={} copied={} masked={} symlinks_skipped={} "
                   "special_skipped={} dirs={}",
                   stats->linked, stats->copied, stats->masked,
                   stats->symlinks_skipped, stats->special_skipped,
                   stats->dirs);
      scratch->release(); // keep on disk; clean up manually
    });
  }

  // __overlay — create a qcow2 overlay on a backing image
  {
    static std::string backing, overlay;
    auto *c = app.add_subcommand("__overlay", "debug: create qcow2 overlay")
                  ->group("");
    c->add_option("backing", backing)->required();
    c->add_option("overlay", overlay)->required();
    c->callback([] {
      if (auto r = Snapshot::create_overlay(backing, overlay); !r)
        std::println(stderr, "error: {}", r.error().message);
      else
        std::println("created {}", overlay);
    });
  }

  // __info — show an overlay's backing chain (prints to stdout directly)
  {
    static std::string overlay;
    auto *c = app.add_subcommand("__info", "debug: qemu-img info")->group("");
    c->add_option("overlay", overlay)->required();
    c->callback([] {
      if (auto r = Snapshot::overlay_info(overlay); !r)
        std::println(stderr, "error: {}", r.error().message);
    });
  }

  // __boot — boot an overlay (serial on stdio; Ctrl-A X to quit qemu)
  {
    static std::string image, kernel, src, out;
    auto *c = app.add_subcommand("__boot", "debug: boot an overlay")->group("");
    c->add_option("--image", image)->required();
    c->add_option("--kernel", kernel)->required();
    c->add_option("--src", src)->required();
    c->add_option("--out", out)->required();
    c->callback([] {
      QemuConfig cfg;
      cfg.image = fs::absolute(image);
      cfg.kernel = fs::absolute(kernel);
      cfg.src_share = fs::absolute(src);
      cfg.out_share = fs::absolute(out);
      cfg.accel = detect_accel().accel;

      std::string cmd;
      for (const auto &a : Vm::build_argv(cfg)) {
        cmd += a;
        cmd += ' ';
      }
      std::println("launching: {}\n", cmd);

      auto vm = Vm::launch(cfg);
      if (!vm) {
        std::println(stderr, "error: {}", vm.error().message);
        return;
      }
      std::println("[booted pid {} — Ctrl-A X to quit qemu]", vm->pid());
      auto code = vm->wait();
      if (!code)
        std::println(stderr, "error: {}", code.error().message);
      else
        std::println("\n[qemu exited: {}]", *code);
    });
  }

  // __exec — boot + connect + run a command (cooked pump, no raw mode)
  {
    static std::string image, kernel, src, out;
    static std::vector<std::string> cmd;
    auto *c = app.add_subcommand("__exec", "debug: run a command in the guest")
                  ->group("");
    c->add_option("--image", image)->required();
    c->add_option("--kernel", kernel)->required();
    c->add_option("--src", src)->required();
    c->add_option("--out", out)->required();
    c->add_option("cmd", cmd, "command after --")->required();
    c->callback([] {
      QemuConfig cfg;
      cfg.image = fs::absolute(image);
      cfg.kernel = fs::absolute(kernel);
      cfg.src_share = fs::absolute(src);
      cfg.out_share = fs::absolute(out);
      cfg.accel = detect_accel().accel;
      cfg.serial_log = fs::absolute("readonly-serial.log");

      auto vm = Vm::launch(cfg);
      if (!vm) {
        std::println(stderr, "boot error: {}", vm.error().message);
        return;
      }
      std::println(stderr, "[booting; serial -> readonly-serial.log]");

      auto vs = VsockClient::connect(cfg.guest_cid, shared::kVsockPort);
      if (!vs) {
        std::println(stderr, "connect error: {}", vs.error().message);
        vm->kill();
        return;
      }

      std::string joined;
      for (std::size_t i = 0; i < cmd.size(); ++i) {
        if (i)
          joined += ' ';
        joined += cmd[i];
      }
      if (auto r = vs->send_run(joined); !r) {
        std::println(stderr, "{}", r.error().message);
        vm->kill();
        return;
      }

      pollfd fds[2];
      fds[0] = {STDIN_FILENO, POLLIN, 0};
      fds[1] = {vs->fd(), POLLIN, 0};
      int exit_code = -1;
      bool done = false;
      while (!done) {
        if (::poll(fds, 2, -1) < 0) {
          if (errno == EINTR)
            continue;
          break;
        }
        if (fds[0].revents & POLLIN) {
          std::array<std::byte, 4096> b;
          ssize_t n = ::read(STDIN_FILENO, b.data(), b.size());
          if (n > 0)
            (void)vs->send_stdin({b.data(), static_cast<std::size_t>(n)});
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
          auto f = vs->next_frame();
          if (!f) {
            std::println(stderr, "\n[stream ended: {}]", f.error().message);
            break;
          }
          if (f->type == shared::FrameType::Data)
            ::write(STDOUT_FILENO, f->data.data(), f->data.size());
          else if (f->type == shared::FrameType::Exit) {
            exit_code = f->exit_code;
            done = true;
          }
        }
      }
      vm->kill();
      std::println(stderr, "\n[guest command exited: {}]", exit_code);
    });
  }

  // __run — interactive session with raw mode + gold bar (the real experience)
  {
    static std::string image, kernel, src, out, cmd;
    auto *c =
        app.add_subcommand("__run", "debug: interactive session in the guest")
            ->group("");
    c->add_option("--image", image)->required();
    c->add_option("--kernel", kernel)->required();
    c->add_option("--src", src)->required();
    c->add_option("--out", out)->required();
    c->add_option("--cmd", cmd, "command to run in guest")->required();
    c->callback([] {
      QemuConfig cfg;
      cfg.image = fs::absolute(image);
      cfg.kernel = fs::absolute(kernel);
      cfg.src_share = fs::absolute(src);
      cfg.out_share = fs::absolute(out);
      cfg.accel = detect_accel().accel;
      cfg.serial_log = fs::absolute("readonly-serial.log");

      auto vm = Vm::launch(cfg);
      if (!vm) {
        std::println(stderr, "boot error: {}", vm.error().message);
        return;
      }

      auto vs = VsockClient::connect(cfg.guest_cid, shared::kVsockPort);
      if (!vs) {
        std::println(stderr, "connect error: {}", vs.error().message);
        vm->kill();
        return;
      }
      if (auto r = vs->send_run(cmd); !r) {
        std::println(stderr, "{}", r.error().message);
        vm->kill();
        return;
      }

      auto term = readonly::core::TerminalSession::enter();
      if (!term) {
        std::println(stderr, "term error: {}", term.error().message);
        vm->kill();
        return;
      }

      readonly::core::Result<int> code =
          readonly::core::fail("session did not run");
      {
        auto session = std::move(*term);
        code = session.pump(*vs);
      }

      vm->kill();
      if (!code)
        std::println(stderr, "session error: {}", code.error().message);
      else
        std::println("[exited: {}]", *code);
    });
  }
}

} // namespace

int main(int argc, char **argv) {
  // Agent-run form: first token is present, not a flag, not a reserved verb.
  if (argc >= 2 && argv[1][0] != '-' && !Registry::is_reserved(argv[1]))
    return run_agent_form(argc, argv);

  CLI::App app{"readonly | run AI coding agents in a read-only VM"};
  app.require_subcommand(0, 1);

  // setup — download base image + kernel from S3 into ~/.readonly (not yet
  // implemented)
  app.add_subcommand("setup", "download the base image and prepare ~/.readonly")
      ->callback([] {
        std::println(
            stderr,
            "setup: not yet implemented — "
            "for now, copy base.qcow2 and bzImage into ~/.readonly manually");
      });

  // install — install + authenticate an agent, then snapshot
  std::string install_cmd;
  std::string name_override;
  {
    auto *c = app.add_subcommand(
        "install", "install + authenticate an agent, then snapshot");
    c->add_option("cmd", install_cmd, "vendor install command (single-quoted)")
        ->required();
    c->add_option("--name", name_override, "override the inferred agent name");
    c->callback([&] {
      auto paths = Paths::discover();
      if (!paths) {
        std::println(stderr, "error: {}", paths.error().message);
        return;
      }
      Registry reg{*paths};
      AgentManager mgr{*paths, reg};

      std::optional<std::string> nm;
      if (!name_override.empty())
        nm = name_override;

      if (auto r = mgr.install(install_cmd, nm); !r)
        std::println(stderr, "install failed: {}", r.error().message);
      else
        std::println("installed.");
    });
  }

  // list — installed agents
  app.add_subcommand("list", "list installed agents")->callback([] {
    auto paths = Paths::discover();
    if (!paths) {
      std::println(stderr, "error: {}", paths.error().message);
      return;
    }
    Registry reg{*paths};
    auto agents = reg.list();
    if (!agents) {
      std::println(stderr, "error: {}", agents.error().message);
      return;
    }
    if (agents->empty()) {
      std::println("no agents installed");
      return;
    }
    for (const auto &a : *agents)
      std::println("{:<16} {}", a.name,
                   a.surface == shared::Surface::Gui ? "gui" : "cli");
  });

  add_debug_commands(app); // remove once `setup` ships

  CLI11_PARSE(app, argc, argv);
  return 0;
}
