use crate::platform::Platform;
use anyhow::{Context, Result};
use std::path::PathBuf;
use std::process::Command;
use which::which;

pub struct RunConfig {
    pub base_image: PathBuf,   // bootable Alpine image with agents installed
    pub project_view: PathBuf, // filtered tree, masks already absent (shared ro)
    pub agent_cmd: String,     // e.g. "claude"
    pub memory_mb: u32,
}

pub fn build_run_command(p: &Platform, cfg: &RunConfig) -> Result<Command> {
    let qemu = which(p.qemu).with_context(|| format!("{} not found in PATH", p.qemu))?;
    let mut c = Command::new(qemu);

    c.args(["-machine", &format!("{},accel={}", p.machine, p.accel)]);
    c.args(["-m", &cfg.memory_mb.to_string()]);
    c.arg("-nographic");

    // Base image, made effectively read-only by -snapshot: all guest writes go
    // to a throwaway overlay, the base file is never touched. This IS the
    // read-only-base + ephemeral-overlay model, in one flag.
    c.args(["-drive", &format!(
        "file={},if=virtio,format=raw", cfg.base_image.display())]);
    c.arg("-snapshot");

    // Project: the filtered tree (masked paths already gone), shared read-only.
    // readonly=on is host-enforced; the guest cannot write even if it remounts.
    c.args(["-fsdev", &format!(
        "local,id=proj,path={},security_model=none,readonly=on",
        cfg.project_view.display())]);
    c.args(["-device", "virtio-9p-pci,fsdev=proj,mount_tag=project"]);

    // Open network: user-mode NAT, zero config, identical on KVM/HVF/WHPX.
    c.args(["-netdev", "user,id=n0"]);
    c.args(["-device", "virtio-net-pci,netdev=n0"]);

    // Tell the in-guest runner (installed at setup) which agent to launch.
    c.args(["-fw_cfg", &format!("name=opt/readonly/cmd,string={}", cfg.agent_cmd)]);

    // NOTE: aarch64 'virt' needs UEFI firmware to boot the disk; the edk2 path
    // gets wired here once setup stages it. (p.needs_uefi)
    Ok(c)
}

pub fn render(c: &Command) -> String {
    let prog = c.get_program().to_string_lossy();
    let args: Vec<String> = c.get_args().map(|a| {
        let s = a.to_string_lossy();
        if s.contains(' ') || s.contains(',') { format!("'{s}'") } else { s.into_owned() }
    }).collect();
    format!("{prog} {}", args.join(" "))
}
