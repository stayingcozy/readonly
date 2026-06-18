mod cli; mod firmware; mod install; mod mask; mod platform; mod setup; mod vm;

use clap::Parser;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
use cli::{Cli, Cmd, RunArgs};
use std::path::PathBuf;

fn data_dir() -> PathBuf {
    if cfg!(windows) {
        PathBuf::from(std::env::var("LOCALAPPDATA").unwrap_or_else(|_| ".".into()))
            .join("readonly")
    } else {
        std::env::var("XDG_DATA_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|_| {
                PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| ".".into()))
                    .join(".local/share")
            })
            .join("readonly")
    }
}
fn main() -> Result<()> {
    let cli = Cli::parse();
    let p = platform::detect()?;
    match cli.cmd {
        Cmd::Setup { force } => setup::run(&p, &data_dir(), force),
        Cmd::Install { cmd } => install::run(&p, &data_dir(), &cmd.join(" ")),
        Cmd::Run(argv) => cmd_run(&p, RunArgs::parse_from(argv)),
    }
}

fn cmd_run(p: &platform::Platform, args: RunArgs) -> Result<()> {
    let project = args.path.canonicalize()?;
    setup::ensure_tools(p)?;

    let masks = mask::resolve_masks(args.no_mask, &args.mask);
    let globset = mask::build_globset(&masks)?;

    let view = tempfile::tempdir()?;
    mask::build_filtered_share(&project, &globset, view.path())?;

    let cfg = vm::RunConfig {
        base_image: data_dir().join("base.qcow2"),  // was base.img
        project_view: view.path().to_path_buf(),
        agent_cmd: args.agent.clone(),
        memory_mb: 1024,
    };
    let fw = firmware::resolve(p)?;               // keep alive until vm exists
    let cmd_file = tempfile::NamedTempFile::new()?; // holds the agent command
    std::fs::write(cmd_file.path(), &cfg.agent_cmd)?;
    let mut cmd = vm::build_run_command(p, &cfg, &fw.args, cmd_file.path())?;

    if args.dry_run {
        println!("agent      : {}", args.agent);
        println!("project(ro): {}", project.display());
        println!("base image : {}", cfg.base_image.display());
        println!("network    : open");
        println!("masks      :");
        if masks.is_empty() { println!("  (none)"); }
        else { for m in &masks { println!("  {m}"); } }
        println!("\nqemu:\n  {}", vm::render(&cmd));
        return Ok(());
    }
    let status = cmd.status()?;
    std::process::exit(status.code().unwrap_or(1));
}
