#include "readonly/core/config.hpp"
#include "readonly/core/mounts.hpp"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>

using namespace readonly::core;
namespace fs = std::filesystem;

namespace {
void touch(const fs::path &p, std::string_view data = "x") {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << data;
}
} // namespace

TEST_SUITE("mirror") {

  TEST_CASE("masked hardlink mirror: excludes .env fam, skip symlinks") {
    auto tmp = fs::temp_directory_path() / "ro_test_mirror";
    fs::remove_all(tmp);

    auto src = tmp / "project";
    touch(src / "main.cpp", "int main(){}");
    touch(src / ".env", "SECRET=1");
    touch(src / ".env.local", "SECRET=2");
    touch(src / "sub/util.cpp");
    fs::create_symlink("main.cpp", src / "link.cpp");

    auto paths = Paths::at(tmp / "root", tmp / "scratch");
    REQUIRE(paths);
    auto scratch = RunScratch::create(*paths);
    REQUIRE(scratch);

    auto stats = scratch->mirror_source(src, MaskSpec{});
    REQUIRE(stats);

    CHECK(stats->masked == 2); // .env, .env.local
    CHECK(stats->symlinks_skipped == 1);
    CHECK(stats->linked >= 2); // main.cpp, sub/util.cpp
    CHECK(stats->dirs >= 1);   // sub/

    CHECK(fs::exists(scratch->src() / "main.cpp"));
    CHECK(fs::exists(scratch->src() / "sub/util.cpp"));

    CHECK_FALSE(fs::exists(scratch->src() / ".env"));
    CHECK_FALSE(fs::exists(scratch->src() / ".env.local"));
    CHECK_FALSE(fs::exists(scratch->src() / "link.cpp"));

    fs::remove_all(tmp);
  }
}
