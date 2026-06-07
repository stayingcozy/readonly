use anyhow::{bail, Result};
use std::env::consts::{ARCH, OS};

#[derive(Debug, Clone, Copy)]
pub enum Arch { X86_64, Aarch64 }

#[derive(Debug, Clone)]
pub struct Platform {
    pub qemu: &'static str,    // qemu binary name
    pub machine: &'static str, // q35 (x86) / virt (aarch64)
    pub accel: &'static str,   // preferred:fallback, e.g. kvm:tcg
    pub console: &'static str, // ttyS0 (x86) / ttyAMA0 (aarch64)
    pub arch: Arch,
    pub needs_uefi: bool,      // aarch64 virt needs edk2 firmware to boot a disk
}

pub fn detect() -> Result<Platform> {
    let arch = match ARCH {
        "x86_64" => Arch::X86_64,
        "aarch64" | "arm64" => Arch::Aarch64,
        other => bail!("unsupported architecture: {other}"),
    };
    let accel = match OS {
        "linux"   => "kvm:tcg",
        "macos"   => "hvf:tcg",
        "windows" => "whpx:tcg",
        other     => bail!("unsupported OS: {other}"),
    };
    Ok(match arch {
        Arch::X86_64 => Platform {
            qemu: "qemu-system-x86_64", machine: "q35", accel,
            console: "ttyS0", arch, needs_uefi: false,
        },
        Arch::Aarch64 => Platform {
            qemu: "qemu-system-aarch64", machine: "virt", accel,
            console: "ttyAMA0", arch, needs_uefi: true,
        },
    })
}
