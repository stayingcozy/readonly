use crate::platform::Platform;
use crate::setup::ensure_tools;
use anyhow::{bail, Context, Result};
use std::path::Path;
use std::process::Command;
use which::which;

/// Built-in package-install recipes. Login is done interactively afterward.
fn recipe(agent: &str) -> Option<&'static str> {
    match agent {
        // VERIFY: package names/commands drift — confirm against the agent's docs.
        // (No commas allowed in the value, or they must be doubled for fw_cfg.)
        "claude" => Some("npm install -g @anthropic-ai/claude-code"),
        // "codex" => Some("npm install -g @openai/codex"),
        _ => None,
    }
}

pub fn run(p: &Platform, data: &Path, agent: &str) -> Result<()> {
    let base = data.join("base.qcow2");
    if !base.exists() {
        bail!("No base VM found. Run `readonly setup` first.");
    }
    
    ensure_tools(p)?;
    let qemu = which(p.qemu)?;
    let fw = crate::firmware::resolve(p)?;
    let mut c = Command::new(qemu);
    c.args(["-machine", &format!("{},accel={}", p.machine, p.accel)]);
    c.args(["-m", "1024"]);
    c.args(["-nographic"]);
    c.args(&fw.args);
    c.args(["-drive", &format!("file={},if=virtio,format=qcow2", base.display())]);
    
    match recipe(agent) {
        Some(cmd) => {
            c.args(["-fw_cfg", &format!("name=opt/readonly/install,string={}", cmd)]);
            println!("Installing '{agent}' into the base VM...");
        }
        None => {
            println!("No built-in recipe for '{agent}'. You'll get a shell — \
                      install it and log in manually.");
        }
    }
    println!("When you've installed and logged in, type `poweroff` in the VM to save.\n");

    let status = c.status().context("running install VM")?;
    if !status.success() {
        bail!("install VM exited with {status}");
    }
    println!("'{agent}' is now in the base image. Run it with:  readonly {agent}");
    Ok(())
}
