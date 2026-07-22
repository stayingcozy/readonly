#pragma once 
#include <filesystem>
#include "readonly/core/error.hpp"

namespace readonly::core {
namespace fs = std::filesystem;

struct Snapshot {
    // qemu-image create -f qcow2 -b <backing> -F qcow2 <overlay>
    static Result<void> create_overlay(const fs::path& backing, const fs::path& overlay);
    static Result<void> discard(const fs::path& overlay);     // remove file
};
} // namespace readonly::core