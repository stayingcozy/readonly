#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "readonly/core/error.hpp"

namespace readonly::core {
namespace fs = std::filesystem;

// Resolves ~/.readonly and ~/.readonly-vm; creates them if missing.
class Paths {
public:
    static Result<Paths> discover();  // named constructor to return Result
    fs::path root() const;            // ~/.readonly
    fs::path base_image() const;      // ~/.readonly/base.qcow2
    fs::path agents_dir() const;      // ~/.readonly/agents 
    fs::path scratch_root() const;    // ~/.readonly-vm
    fs::path state_file() const;      // ~/.readonly/aivm.state
private:
    fs::path root_;
    fs::path scratch_;
}

// ~/.readonly/config, key=value. Optional accel override, default mask.
struct GlobalConfig {
    std::vector<std::string> default_mask{".env"};
    std::string accel_override;   // empty = autodetect
    static Result<GlobalConfig> load(const Paths&);
    Result<void> save(const Paths&) const;
};

} // namespace readonly::core