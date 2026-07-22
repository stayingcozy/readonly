#pragma once 
#include <filesystem> 
#include <optional> 
#include <string>
#include <string_view>
#include "readonly/core/config.hpp"
#include "readonly/core/mounts.hpp"
#include "readonly/core/registry.hpp"
#include "readonly/core/error.hpp"

namespace readonly::core {
namespace fs = std::filesystem;

class AgentManager {
public:
    AgentManager(Paths paths, Registry registry);

    // boot base + network -> run install_cmd under interactive PTY ->
    // user authentications -> clean poweroff -> keep overlay + write .meta
    Result<void> install(std::string_view install_cmd,
                            std::optional<std::string> name = std::nullopt);

    // ephemeral overaly on agent snapshot -> masked mirror -> run under PTY -> discard
    Result<int> run(std::string_view name, const fs::path& target, const MaskSpec&);

    // "anthropic/claude" -> "claude";"codex" -> "codex"; else derive or random
    static std::string infer_name(std::string_view install_cmd);
private:
        Paths    paths_;
        Registry registry_;
};
} // namespace readonly::core