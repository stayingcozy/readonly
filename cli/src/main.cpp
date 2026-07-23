#include <CLI/CLI.hpp>
#include <string>
#include "readonly/core/registry.hpp"

using readonly::core::Registry;

// Anything that isn't a reserved verb (and isn't a flag) is an agent name
static int run_agent_form(int argc, char** argv) {
    CLI::App app{"run an installed agent"};
    std::string name, path{"."};
    std::string mask{".env"};
    app.add_option("name", name)->required();
    app.add_option("path", path);
    app.add_option("--mask", mask);
    CLI11_PARSE(app, argc, argv);
    // TODO: Paths::discover -> Registry -> AgentManager::run(name, path, mask)
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1][0] != '-' && !Registry::is_reserved(argv[1]))
        return run_agent_form(argc, argv);

    CLI::App app{"readonly - run AI coding agents in an ephemeral read-only VM"};
    app.require_subcommand(0, 1);

    auto* setup = app.add_subcommand("setup", "download base image, prepare ~/.readonly");
    (void)setup; // TODO callback: pull base.qcow2 from S3/github

    std::string install_cmd, name_override;
    auto* install = app.add_subcommand("install", "install + authenticate an agent, then snapshot");
    install->add_option("cmd", install_cmd, "vendor install command (single-quoted)")->required();
    install->add_option("--name", name_override, "override the inferred agent name");
    // TODO callback: AgentManager::install(install_cmd, name_override?)

    auto* list = app.add_subcommand("list", "list installed agents");
    (void)list;   // TODO callback: Registry::list

    CLI11_PARSE(app, argc, argv);
    return 0;
}