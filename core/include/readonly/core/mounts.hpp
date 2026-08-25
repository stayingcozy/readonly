#pragma once
#include "readonly/core/config.hpp"
#include "readonly/core/error.hpp"
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace readonly::core {
namespace fs = std::filesystem;

struct MaskSpec {
  std::vector<std::string> names{".env"}; // v1 fixed default; --mask overrides
  // git-aware selection - fast-follow that replaces the plain denylist
  // Matches a directory-entry basename => excluded from the mirror
  // ".env" masks ".env" and the dotted family (".env.local", etc)
  bool matches(std::string_view basename) const;
};

struct MirrorStats {
  std::size_t linked{0};
  std::size_t copied{0};
  std::size_t masked{0};
  std::size_t symlinks_skipped{0};
  std::size_t special_skipped{0}; // device / fifo / socket
  std::size_t dirs{0};
};

// Owns ~/.readonly-vm/<run-id>/{src, out}; rm -rf on destruction
class RunScratch {
public:
  static Result<RunScratch> create(const Paths &);
  Result<MirrorStats>
  mirror_source(const fs::path &target,
                const MaskSpec &); // hardlink mirror, minus mask

  const fs::path &src() const { return src_; } // export readonly=on
  const fs::path &out() const { return out_; } // export read-write
  const fs::path &run_dir() const { return run_dir_; }

  fs::path release(); // give up ownership (no cleanup); returns run_dir

  ~RunScratch();
  RunScratch(RunScratch &&) noexcept;
  RunScratch &operator=(RunScratch &&) noexcept;
  RunScratch(const RunScratch &) = delete;
  RunScratch &operator=(const RunScratch &) = delete;

private:
  RunScratch() = default;
  void cleanup() noexcept;
  fs::path run_dir_, src_, out_;
};
} // namespace readonly::core