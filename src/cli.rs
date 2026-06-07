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
    /// One-time: build the base VM (fetch Alpine, kernel, Node runtime).
    Setup {
        /// Rebuild even if a base image exist
        #[arg(long)]
        force: bool,
    },
    /// One-time per agent: install an agent CLI into the base VM + log in.
    Install { agent: String },
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
