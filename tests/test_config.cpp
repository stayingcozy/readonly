#include "readonly/core/config.hpp"
#include "readonly/core/vm.hpp"
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>

using namespace readonly::core;
namespace fs = std::filesystem;

TEST_SUITE("config") {
  TEST_CASE("expand_user resolves ~ against HOME") {
    auto p = Paths::expand_user("~/foo/bar");
    REQUIRE(p);
    CHECK(p->string() == std::string(std::getenv("HOME")) + "/foo/bar");
  }

  TEST_CASE("Paths::at creates root + scratch and derives child paths") {
    auto tmp = fs::temp_directory_path() / "ro_test_paths";
    fs::remove_all(tmp);

    auto paths = Paths::at(tmp / "root", tmp / "scratch");
    REQUIRE(paths);
    CHECK(fs::exists(paths->root()));
    CHECK(fs::exists(paths->scratch_root()));
    CHECK(paths->base_image() == paths->root() / "base.qcow2");

    fs::remove_all(tmp);
  }

  TEST_CASE("GlobalConfig: default when absent, then round-trips") {
    auto tmp = fs::temp_directory_path() / "ro_test_cfg";
    fs::remove_all(tmp);

    auto paths = Paths::at(tmp / "root", tmp / "scratch");
    REQUIRE(paths);

    auto def = GlobalConfig::load(*paths);
    REQUIRE(def);
    CHECK(def->default_mask == std::vector<std::string>{".env"});
    CHECK(def->accel_override.empty());

    GlobalConfig c;
    c.default_mask = {".env", "secrets"};
    c.accel_override = "tcg";
    REQUIRE(c.save(*paths));

    auto rt = GlobalConfig::load(*paths);
    REQUIRE(rt);
    CHECK(*rt == c);

    fs::remove_all(tmp);
  }
}

TEST_SUITE("accel") {

  TEST_CASE("detect_accel is stable and returns valid accel") {
    auto a = detect_accel();
    CHECK(a.accel == detect_accel().accel);

    bool valid = a.accel == Accel::Kvm || a.accel == Accel::Hvf ||
                 a.accel == Accel::Whpx || a.accel == Accel::None;
    CHECK(valid);

#ifdef __linux__
    // if no on linux kvm is unuseable
    if (a.accel == Accel::None)
      CHECK((!fs::exists("/dev/kvm") || a.kvm_present_but_denied));
#endif
  }
}
