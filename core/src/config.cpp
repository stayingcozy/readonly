#include "readonly/core/config.hpp"
#include "kv.hpp"

#include <cstdlib>
#include <format>
#include <pwd.h>
#include <unistd.h>

namespace readonly::core {

namespace {
Result<fs::path> home_dir() {
  if (const char *h = std::getenv("HOME"); h && *h)
    return fs::path{h};
  if (const passwd *pw = ::getpwuid(::getuid());
      pw && pw->pw_dir && *pw->pw_dir)
    return fs::path{pw->pw_dir};
  return fail(
      "cannot determine home directory (HOME unset and no passwd entry)");
}

Result<void> ensure_dir(const fs::path &p) {
  std::error_code ec;
  fs::create_directories(p, ec);
  if (ec && !fs::is_directory(p, ec))
    return fail(std::format("cannot create {}: {}", p.string(), ec.message()));
  return {};
}
} // namespace

Result<Paths> Paths::at(fs::path root, fs::path scratch) {
  Paths p;
  p.root_ = fs::absolute(std::move(root));
  p.scratch_ = fs::absolute(std::move(scratch));

  if (auto r = ensure_dir(p.root_); !r)
    return std::unexpected(r.error());
  if (auto r = ensure_dir(p.root_ / "agents"); !r)
    return std::unexpected(r.error());
  if (auto r = ensure_dir(p.scratch_); !r)
    return std::unexpected(r.error());
  return p;
}

Result<Paths> Paths::discover() {
  auto home = home_dir();
  if (!home)
    return std::unexpected(home.error());
  return Paths::at(*home / ".readonly", *home / ".readonly-vm");
}

fs::path Paths::root() const { return root_; }
fs::path Paths::scratch_root() const { return scratch_; }
fs::path Paths::base_image() const { return root_ / "base.qcow2"; }
fs::path Paths::agents_dir() const { return root_ / "agents"; }
fs::path Paths::state_file() const { return root_ / "aivm.state"; }

Result<GlobalConfig> GlobalConfig::load(const Paths &paths) {
  const fs::path file = paths.root() / "config";
  if (!fs::exists(file))
    return GlobalConfig{}; // first run: silent defaults

  auto text = kv::read_file(file);
  if (!text)
    return std::unexpected(text.error());

  auto entries = kv::parse_kv(*text, file);
  if (!entries)
    return std::unexpected(entries.error());

  GlobalConfig cfg;
  for (const auto &[key, value] : *entries) {
    if (key == "default_mask")
      cfg.default_mask = kv::split_list(value);
    else if (key == "accel_override")
      cfg.accel_override = value;
    // unknown keys ignored: old binaries tolerate new configs
  }
  return cfg;
}

Result<void> GlobalConfig::save(const Paths &paths) const {
  const std::string body =
      std::format("# readonly global config \n"
                  "default_mask={}\n"
                  "accel_override={}\n",
                  kv::join_list(default_mask), accel_override);
  return kv::write_file_atomic(paths.root() / "config", body);
}

} // namespace readonly::core