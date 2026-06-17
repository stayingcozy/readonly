use clap::{Parser, Subcommand};
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(name = "readonly", about = "Run AI agents in a read-only, scoped microVM")]
pub struct Cli {
    #[command(subcommand)]
    pub cmd: Cmd,
}

#[derive(Subcommand, Debug)]
pub enum Cmd {
    /// One-time: build the base VM (fetch Debian, install tools, bake the runner).
    Setup {
        /// Rebuild even if a base image exist
        #[arg(long)]
        force: bool,
    },
    /// One-time: install an agent CLI by running <cmd> verbatim in the base VM,
    /// then log in. Quote it if it contains a pipe, e.g.
    ///   readonly install 'curl -fsSL https://claude.ai/install.sh | bash'
    Install {
        #[arg(trailing_var_arg = true, allow_hyphen_values = true, required = true)]
        cmd: Vec<String>,
    },
    /// Run an agent: `readonly claude [PATH] [--mask ..] [--no-mask] [--dry-run]`
    #[command(external_subcommand)]
    Run(Vec<String>),
}

/// Parsed from the external-subcommand argv (first element is the agent).
#[derive(Parser, Debug)]
#[command(no_binary_name = true)]
pub struct RunArgs {
    pub agent: String,
    #[arg(default_value = ".")]
    pub path: PathBuf,
    #[arg(long)]
    pub mask: Vec<String>,
    #[arg(long)]
    pub no_mask: bool,
    #[arg(long)]
    pub dry_run: bool,
}
