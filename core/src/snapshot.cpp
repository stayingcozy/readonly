#include "readonly/core/snapshot.hpp"
#include "proc.hpp"

#include <format>
#include <system_error>

namespace readonly::core {

Result<void> Snapshot::qemu_img_available() {
  auto r = proc::run({"qemu-img", "--version"});
  if (!r || r->exit_code != 0)
    return fail("qemu-img not found on PATH - install QEMU tools (Fedora: sudo "
                "dnf install qemu-img)");
  return {};
}

Result<void> Snapshot::create_overlay(const fs::path &backing,
                                      const fs::path &overlay) {
  if (auto q = qemu_img_available(); !q)
    return std::unexpected(q.error());

  std::error_code ec;

  // backing must exist and resolve to abs. path
  const fs::path backing_abs = fs::canonical(backing, ec);
  if (ec)
    return fail(std::format("backing image {} not found: {}", backing.string(),
                            ec.message()));

  if (fs::exists(overlay))
    return fail(std::format("overlay {} already exists (refusing to overwrite)",
                            overlay.string()));

  fs::create_directories(overlay.parent_path(), ec);
  if (ec && !fs::is_directory(overlay.parent_path())) {
    return fail(std::format("cannot create {}: {}",
                            overlay.parent_path().string(), ec.message()));
  }

  auto r = proc::run({"qemu-img", "create", "-f", "qcow2", "-b",
                      backing_abs.string(), "-F", "qcow2", overlay.string()});
  if (!r)
    return std::unexpected(r.error());
  if (r->exit_code != 0)
    return fail(
        std::format("qemu-img create failed: {}",
                    r->stderr_text.empty() ? "(no output)" : r->stderr_text));

  return {};
}

Result<void> Snapshot::discard(const fs::path &overlay) {
  std::error_code ec;
  fs::remove(overlay, ec); // missing file .. returns false, ec unset
  if (ec)
    return fail(
        std::format("cannot remove {}: {}", overlay.string(), ec.message()));
  return {};
}

Result<std::string> Snapshot::overlay_info(const fs::path &overlay) {
  auto r = proc::run({"qemu-img", "info", overlay.string()});
  if (!r)
    return std::unexpected(r.error());
  if (r->exit_code != 0)
    return fail(std::format("qemu-img info failed: {}", r->stderr_text));
  return r->stderr_text;
}

} // namespace readonly::core
