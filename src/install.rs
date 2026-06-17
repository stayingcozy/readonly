use crate::platform::Platform;
use crate::setup::ensure_tools;
use anyhow::{bail, Context, Result};
use std::path::Path;
use std::process::Command;
use tempfile::NamedTempFile;
use which::which;

/// Install an agent CLI into the base image by running `install_cmd` inside the
/// VM verbatim. No per-provider recipes: the user pastes whatever the agent's
/// official docs give them, e.g.
///   readonly install 'curl -fsSL https://claude.ai/install.sh | bash'
/// After it runs, the in-guest runner drops to a shell to log in; `poweroff`
/// saves the result into the base image.
pub fn run(p: &Platform, data: &Path, install_cmd: &str) -> Result<()> {
    let base = data.join("base.qcow2");
    if !base.exists() {
        bail!("No base VM found. Run `readonly setup` first.");
    }

    ensure_tools(p)?;
    let qemu = which(p.qemu)?;
    let fw = crate::firmware::resolve(p)?; // keep alive until the VM exits

    // Hand the command to the guest through a file (read by the runner via
    // fw_cfg `file=`), never `-fw_cfg ...,string=`. This keeps commas, quotes,
    // and pipes in the pasted command from colliding with QEMU's option parser.
    let cmd_file = NamedTempFile::new().context("creating install-command file")?;
    std::fs::write(cmd_file.path(), install_cmd).context("writing install command")?;

    let mut c = Command::new(qemu);
    c.args(["-machine", &format!("{},accel={}", p.machine, p.accel)]);
    c.args(["-m", "1024"]);
    c.arg("-nographic");
    c.args(&fw.args);
    // Boot the base read-WRITE (no -snapshot): the install must persist.
    c.args(["-drive", &format!("file={},if=virtio,format=qcow2", base.display())]);
    // The installer fetches over the network, so give it open user-mode NAT.
    c.args(["-netdev", "user,id=n0"]);
    c.args(["-device", "virtio-net-pci,netdev=n0"]);
    c.args(["-fw_cfg", &format!(
        "name=opt/readonly/install,file={}", cmd_file.path().display())]);

    println!("Installing into the base VM:\n  {install_cmd}\n");
    println!("When it finishes, log in to your agent, then type `poweroff` to save.\n");

    let status = c.status().context("running install VM")?;
    if !status.success() {
        bail!("install VM exited with {status}");
    }
    drop(cmd_file); // explicit: the command file must outlive the running VM
    println!("Done. The agent is now baked into the base image.");
    Ok(())
}
