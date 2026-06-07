mod cli; mod mask; mod setup; mod platform; mod vm;

use anyhow::Result;
use clap::Parser;
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
        Cmd::Setup => cmd_setup(&p),
        Cmd::Install { agent } => cmd_install(&p, &agent),
        Cmd::Run(argv) => cmd_run(&p, RunArgs::parse_from(argv)),
    }
}

fn cmd_run(p: &platform::Platform, args: RunArgs) -> Result<()> {
    let project = args.path.canonicalize()?;
    let masks = mask::resolve_masks(args.no_mask, &args.mask);
    let globset = mask::build_globset(&masks)?;

    let view = tempfile::tempdir()?;
    mask::build_filtered_share(&project, &globset, view.path())?;

    let cfg = vm::RunConfig {
        base_image: data_dir().join("base.img"),
        project_view: view.path().to_path_buf(),
        agent_cmd: args.agent.clone(),
        memory_mb: 512,
    };
    let mut cmd = vm::build_run_command(p, &cfg)?;

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

fn cmd_setup(_p: &platform::Platform) -> Result<()> {
    // TODO (next slice, needs real fetch/boot — can't exercise it here):
    //  1. fetch Alpine minirootfs + virt kernel for this arch into data_dir()
    //  2. qemu-img create base.img; partition + format + unpack rootfs
    //  3. boot base.img read-WRITE (no -snapshot), interactive console, open net
    //  4. inside: apk add nodejs npm; install the in-guest runner + a service
    //     that mounts tag `project` ro at /mnt/project and execs the fw_cfg cmd
    //  5. clean shutdown -> base.img is the sealed appliance
    println!("setup: not yet implemented (foundation slice)");
    Ok(())
}

fn cmd_install(_p: &platform::Platform, agent: &str) -> Result<()> {
    // TODO (next slice): boot base.img read-WRITE + open net + interactive console,
    // run the agent's normal install + login inside (claude recipe first; generic
    // shell fallback otherwise), clean shutdown so it persists into base.img.
    println!("install: not yet implemented for '{agent}'");
    Ok(())
}
