#include "readonly/core/registry.hpp"
#include <array>
#include <algorithm>

namespace readonly::core {

bool Registry::is_reserved(std::string_view name) {
    static constexpr std::array reserved = {
        "setup", "install", "list", "help", "-h", "--help"
    };
    return std::find(reserved.begin(), reserved.end(), name) != reserved.end();
}

} // namespace readonly::core