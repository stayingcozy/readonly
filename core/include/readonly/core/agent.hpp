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

    // boot base + network -> run install_cmd under interactive ->
    // run agent for auth -> clean poweroff+wait -> (atomically) keep overlay + write .meta
    // any failure leaves registry untouched
    Result<void> install(std::string_view install_cmd,
                            std::optional<std::string> name = std::nullopt);

    //Resolve -> mirror source -> throwaway overlay on agent snapshot
    // -> boot -> run agent's run_cmd through terminal -> discard all
    // Return guest command's exit code
    Result<int> run(std::string_view name, const fs::path& target, const MaskSpec&);

    // "anthropic/claude" -> "claude";"codex" -> "codex"; "copilot" -> "copilot"; 
    // else derive or random
    static std::string infer_name(std::string_view install_cmd);

private:
    struct Deps { fs::path base_image; fs::path kernel; };
    Result<Deps> resolve_deps() const; // check base.qcow2 + bzImage exist

    Paths    paths_;
    Registry registry_;
};
} // namespace readonly::core