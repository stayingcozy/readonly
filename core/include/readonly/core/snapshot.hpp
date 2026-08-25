#pragma once
#include "readonly/core/error.hpp"
#include <filesystem>
#include <string>

namespace readonly::core {
namespace fs = std::filesystem;

struct Snapshot {
  // qemu-image create -f qcow2 -b <backing> -F qcow2 <overlay>
  static Result<void> create_overlay(const fs::path &backing,
                                     const fs::path &overlay);

  // Remove overlay file, missing file not error
  static Result<void> discard(const fs::path &overlay);

  // `qemu-img info` text for debug inspection
  static Result<std::string> overlay_info(const fs::path &overlay);

  // check if qemu-img on PATH
  static Result<void> qemu_img_available();
};
} // namespace readonly::core