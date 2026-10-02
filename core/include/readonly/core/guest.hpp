#pragma once
#include "readonly/core/error.hpp"
#include "readonly/core/vm.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace readonly::core {

struct GuestRun {
  std::string output;
  int exit_code{-1};
};

using GuestOutFn = std::function<void(std::string_view)>;

Result<int> exec_in_guest(
    const QemuConfig &cfg, std::string_view cmd, std::string_view stdin_data,
    const GuestOutFn &on_out, int stdin_fd = -1,
    std::chrono::milliseconds idle_timeout = std::chrono::milliseconds{-1});

Result<GuestRun>
run_in_guest(const QemuConfig &cfg, std::string_view cmd,
             std::string_view stdin_data = {},
             std::chrono::milliseconds idle_timeout = std::chrono::seconds{60});

} // namespace readonly::core
