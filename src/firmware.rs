use crate::platform::Platform;
use anyhow::{Context, Result};
use std::fs;
use std::path::{Path, PathBuf};
use tempfile::NamedTempFile;
use which::which;

/// QEMU firmware args, plus any temp files that must outlive the VM.
pub struct Firmware {
    pub args: Vec<String>,
    _vars: Option<NamedTempFile>, // writable UEFI var store; kept alive as a guard
}

const CODE_NAMES: &[&str] =
    &["edk2-aarch64-code.fd", "AAVMF_CODE.fd", "QEMU_EFI.fd", "QEMU_CODE.fd"];
const VARS_NAMES: &[&str] =
    &["edk2-arm-vars.fd", "AAVMF_VARS.fd", "QEMU_VARS.fd"];

/// x86 q35 has built-in SeaBIOS (no args). aarch64 'virt' needs edk2 firmware.
pub fn resolve(p: &Platform) -> Result<Firmware> {
    if !p.needs_uefi {
        return Ok(Firmware { args: vec![], _vars: None });
    }
    let qemu = which(p.qemu)?;
    let dirs = candidate_dirs(&qemu);

    let code = find_in(&dirs, CODE_NAMES).with_context(missing_firmware_msg)?;
    let vars_tpl = find_in(&dirs, VARS_NAMES).with_context(missing_firmware_msg)?;

    // Writable per-boot copy of the var store (we don't persist UEFI vars).
    let tmp = NamedTempFile::new()?;
    fs::copy(&vars_tpl, tmp.path()).context("copying UEFI vars template")?;

    let args = vec![
        "-drive".into(),
        format!("if=pflash,format=raw,unit=0,readonly=on,file={}", code.display()),
        "-drive".into(),
        format!("if=pflash,format=raw,unit=1,file={}", tmp.path().display()),
    ];
    Ok(Firmware { args, _vars: Some(tmp) })
}

fn candidate_dirs(qemu: &Path) -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    // QEMU ships firmware under <prefix>/share/qemu — derive from the binary
    // (covers Homebrew on macOS, the Windows installer, relocatable installs).
    if let Some(prefix) = qemu.parent().and_then(Path::parent) {
        dirs.push(prefix.join("share/qemu"));
        dirs.push(prefix.join("share/edk2/aarch64"));
        dirs.push(prefix.join("share/AAVMF"));
    }
    for d in [
        "/usr/share/qemu",
        "/usr/share/edk2/aarch64",
        "/usr/share/edk2-armvirt/aarch64",
        "/usr/share/AAVMF",
    ] {
        dirs.push(PathBuf::from(d));
    }
    dirs
}

fn find_in(dirs: &[PathBuf], names: &[&str]) -> Option<PathBuf> {
    for d in dirs {
        for n in names {
            let p = d.join(n);
            if p.is_file() {
                return Some(p);
            }
        }
    }
    None
}

fn missing_firmware_msg() -> String {
    "could not locate aarch64 UEFI firmware (edk2).\n\
     It usually ships with QEMU; on Debian/Ubuntu install `qemu-efi-aarch64`.\n\
     Run `qemu-system-aarch64 -L help` to see QEMU's firmware search paths.\n\
     If pflash gives size errors, pad the .fd files to 64 MiB; or fall back to\n\
     `-bios <edk2-aarch64-code.fd>`."
        .to_string()
}
