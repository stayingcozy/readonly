#include "readonly/core/guest.hpp"
#include "readonly/core/snapshot.hpp"
#include "readonly/core/vm.hpp"

#include <algorithm>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <optional>
#include <string>

using namespace readonly::core;
namespace fs = std::filesystem;

namespace {

std::optional<QemuConfig> vm_cfg(unsigned cid) {
  const char *home = std::getenv("HOME");
  if (!home)
    return std::nullopt;

  const char *ei = std::getenv("READONLY_TEST_IMAGE");
  const char *ek = std::getenv("READONLY_TEST_KERNEL");
  fs::path base = ei ? fs::path(ei) : fs::path(home) / ".readonly/base.qcow2";
  fs::path kernel = ek ? fs::path(ek) : fs::path(home) / ".readonly/bzImage";

  if (!fs::is_regular_file(base) || !fs::is_regular_file(kernel))
    return std::nullopt;
  if (!fs::exists("/dev/vhost-vsock"))
    return std::nullopt;
  if (!Snapshot::qemu_img_available())
    return std::nullopt;

  auto tmp = fs::temp_directory_path() / ("ro_it_" + std::to_string(cid));

  fs::remove_all(tmp);
  fs::create_directories(tmp / "src");
  fs::create_directories(tmp / "out");

  auto overlay = tmp / "overlay.qcow2";
  if (!Snapshot::create_overlay(base, overlay))
    return std::nullopt;

  QemuConfig cfg;
  cfg.image = overlay;
  cfg.kernel = kernel;
  cfg.src_share = tmp / "src";
  cfg.out_share = tmp / "out";
  cfg.accel = detect_accel().accel;
  cfg.guest_cid = cid;
  cfg.serial_log = tmp / "serial.log";
  return cfg;
}

} // namespace

TEST_SUITE("integration") {

  TEST_CASE("run_in_guest: output capture, exit codes, stdin" *
            doctest::timeout(240.0)) {
    auto cfg = vm_cfg(0x2a);
    if (!cfg) {
      MESSAGE("no VM image / kernel / vhost-vsock -- skipping");
      return;
    }

    SUBCASE("echo round-trips through the PTY") {
      auto r = run_in_guest(*cfg, "echo hello");
      INFO("error: ", (r ? std::string{} : r.error().message));
      REQUIRE(r);
      CHECK(r->exit_code == 0);
      CHECK(r->output.find("hello") != std::string::npos);
    }

    SUBCASE("non-zero exit code propagates") {
      auto r = run_in_guest(*cfg, "exit 7");
      INFO("error: ", (r ? std::string{} : r.error().message));
      REQUIRE(r);
      CHECK(r->exit_code == 7);
    }

    SUBCASE("stdin is delivered to the guest command") {
      auto r = run_in_guest(*cfg, R"(read line; echo "got:$line")", "ping\n");
      INFO("error: ", (r ? std::string{} : r.error().message));
      REQUIRE(r);
      CHECK(r->exit_code == 0);
      CHECK(r->output.find("got:ping") != std::string::npos);
    }

    SUBCASE("large output reassembles across frames") {
      // exercise length encoding the Data-forwarding path
      auto r = run_in_guest(*cfg, "printf 'x%.0s' $(seq 1 20000)");
      INFO("error: ", (r ? std::string{} : r.error().message));
      REQUIRE(r);
      CHECK(r->exit_code == 0);
      CHECK(std::count(r->output.begin(), r->output.end(), 'x') == 20000);
    }
  }
}
