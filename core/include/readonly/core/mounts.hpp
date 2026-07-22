#pragma once 
#include <filesystem>
#include <string>
#include <vector>
#include "readonly/core/config.hpp"
#include "readonly/core/error.hpp"

namespace readonly::core {
namespace fs = std::filesystem;

struct MaskSpec {
    std::vector<std::string> names{".env"}; // v1 fixed default; --mask overrides
    // git-aware selection - fast-follow that replaces the plain denylist
};

// Owns ~/.readonly-vm/<run-id>/{src, out}; rm -rf on destruction
class RunScratch {
public:
    static Result<RunScratch> create(const Paths&);
    Result<void> mirror_source(const fs::path& target, const MaskSpec&);  // hardlink mirror, minus mask
    const fs::path& src() const;   // export readonly=on
    const fs::path& out() const;   // export read-write
    ~RunScratch();

    RunScratch(RunScratch&&) noexcept;
    RunScratch& operator=(RunScratch&&) noexcept;
    RunScratch(const RunScratch&) = delete;
    RunScratch& operator=(const RunScratch&) = delete;
private:
    fs::path run_dir_, src_, out_;
};
} // namespace readonly::core