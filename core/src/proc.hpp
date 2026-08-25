#pragma once
#include "readonly/core/error.hpp"
#include <string>
#include <vector>

namespace readonly::core::proc {

struct Output {
  int exit_code{-1};
  std::string stderr_text;
};

Result<Output> run(const std::vector<std::string> &argv);

} // namespace readonly::core::proc