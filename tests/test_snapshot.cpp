#include "readonly/core/snapshot.hpp"
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <string>

using namespace readonly::core;
namespace fs = std::filesystem;

TEST_SUITE("overlay") {

  TEST_CASE("create_overlay makes a qcow2 backed by base") {
    if (!Snapshot::qemu_img_available()) {
      MESSAGE("qemu-img not on PATH -- skipping");
      return;
    }

    auto tmp = fs::temp_directory_path() / "ro_test_overlay";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    auto backing = tmp / "base.qcow2";
    REQUIRE(
        std::system(("qemu-img create -f qcow2 " + backing.string() + " 16M")
                        .c_str()) == 0);

    auto overlay = tmp / "overlay.qcow2";
    REQUIRE(Snapshot::create_overlay(backing, overlay));
    CHECK(fs::exists(overlay));

    auto info = Snapshot::overlay_info(overlay);
    REQUIRE(info);
    CHECK(info->find(backing.filename().string()) != std::string::npos);

    CHECK(Snapshot::discard(overlay));
    CHECK_FALSE(fs::exists(overlay));
    CHECK(Snapshot::discard(overlay));

    fs::remove_all(tmp);
  }
}
