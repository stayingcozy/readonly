#pragma once
#include <cstdint>
#include <string>

namespace readonly::shared {

    enum class Surface : std::uint8_t { Cli, Gui };

    // Persist as agents/<name>.meta (key=value lines). Name = filename stem
    struct AgentDescriptor {
        std::string name;     // filesystem-safe, non-reserved
        Surface     surface{Surface::Cli};
        std::string run_cmd;     // comand exec'd in guest on `readonly <name>`
        std::string install_cmd; // original vendor cmd 
        std::string created;     // ISO-8601
    };

} // namespace readonly::shared