use crate::platform::{Arch, Platform};
use anyhow::{bail, Context, Result};
use std::fs;
use std::io::{Read, Write};
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::process::Command;
use which::which;

struct ImageSource {
    url: &'static str,
    sha256: &'static str, // empty = not pinned yet, verification skipped
}

// VERIFY: fill from https://alipinelinux.org/cloud/ (nocloud qcow2 + its sha256
fn image_source(arch: Arch) -> ImageSource {
    match arch {
        Arch::X86_64 => ImageSource {
            url: "https://example/alpine-nocloud-x86_64.qcow2",
            sha256: "",
        },
        Arch::Aarch64 => ImageSource {
            url: "https://example/alpine-nocloud-aarch64.qcow2",
            sha256: "",
        },
    }
}


pub fn run(p: &Platform, data: &Path, force: bool) -> Result<()> {
    ensure_tools(p)?;

    let cache = data.join("cache");
    let base = data.join("base.qcow2");
    fs::create_dir_all(&cache)?;

    if base.exists() && !force {
        println!("Already set up ({}). Use --force to rebuild.", base.display());
        return Ok(());
    }

    // 1. Fetch the pristine bootable image (cached), then copy to the working base.
    let pristine = fetch_image(p, &cache)?;
    fs::copy(&pristine, &base).context("copying base image")?;

    // 2. Build the cloud-init seed in-process (no external ISO/FAT tools).
    let seed = data.join("seed.img");
    write_seed(p, &seed)?;

    // 3. Boot once read-WRITE to provision; cloud-init ends with poweroff.
    println!("Provisioning base VM (installing Node + runner)...");
    provision(p, &base, &seed)?;
    let _ = fs::remove_file(&seed); // seed only needed for first boot

    println!("Setup complete: {}", base.display());
    Ok(())
}

pub(crate) fn ensure_tools(p: &Platform) -> Result<()> {
    require_tool(p.qemu)?;
    require_tool("qemu-img")?;
    Ok(())
}

fn require_tool(name: &str) -> Result<PathBuf> {
    let path = match which(name) {
        Ok(p) => p,
        Err(_) => bail!("{name} not found in PATH.\n\nInstall QEMU:\n{}", install_hint()),
    };
    // Confirm it actually runs (catches wrong-arch / broken installs).
    let out = Command::new(&path)
        .arg("--version")
        .output()
        .with_context(|| format!("failed to execute {}", path.display()))?;
    if !out.status.success() {
        bail!("{} is present but did not run successfully", path.display());
    }
    Ok(path)
}

fn install_hint() -> String {
    match std::env::consts::OS {
        "macos" => "  brew install qemu".to_string(),
        "windows" => "  scoop install qemu  (or https://www.qemu.org/download/#windows)\n  \
                       then ensure the QEMU folder is on your PATH".to_string(),
        "linux" => "  Debian/Ubuntu:  sudo apt install qemu-system qemu-utils\n  \
                     Fedora:         sudo dnf install qemu-system-x86 qemu-img\n  \
                     Arch:           sudo pacman -S qemu-full".to_string(),
        other => format!("  install QEMU for {other} via your package manager"),
    }
}

fn fetch_image(p: &Platform, cache: &Path) -> Result<PathBuf> {
    let dest = cache.join(format!("alpine-{:?}.qcow2", p.arch).to_lowercase());
    if dest.exists() {
        return Ok(dest);
    }
    let src = image_source(p.arch);
    println!("Downloading base image: {}", src.url);

    let resp = ureq::get(src.url).call().context("downloading base image")?;
    let mut reader = resp.into_reader();

    let tmp = dest.with_extension("part");
    let mut file = fs::File::create(&tmp)?;
    let mut hasher = Sha256::new();
    
    let mut buf = [0u8; 64 * 1024];
    let mut total: u64 = 0;
    let mut mark = 16 * 1024 * 1024;
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        file.write_all(&buf[..n])?;
        hasher.update(&buf[..n]);
        total += n as u64;
        if total >= mark {
            eprint!("\r downloaded {} MiB", total / (1024 * 1024));
            mark += 16 * 1024 * 1024;
        }
    }
    file.flush()?;
    eprintln!("\r downloaded {} MiB", total / (1024 * 1024));
    
    let got = hex(&hasher.finalize());
    if src.sha256.is_empty() {
        eprintln!(" warning: no pinned sha256 - skipping verification");
        eprintln!(" (computed {got}; past it into image_source to enable the check)");
    } else if !got.eq_ignore_ascii_case(src.sha256) {
        let _ = fs::remove_file(&tmp);
        bail!(
            "checksum mismatch for base image\n expected {}\n got   {}",
            src.sha256, got
        );
    }

    fs::rename(&tmp, &dest)?;
    Ok(dest)
}

fn hex(bytes: &[u8]) -> String {
    use std::fmt::Write as _;
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        let _ = write!(s, "{b:02x}");
    }
    s
}

fn write_seed(p: &Platform, seed: &Path) -> Result<()> {
    let meta = "instance-id: readonly\nlocal-hostname: readonly\n";
    let user = user_data(p);

    let img = fs::OpenOptions::new()
        .read(true).write(true).create(true).truncate(true)
        .open(seed)?;
    img.set_len(2 * 1024 * 1024)?; // 2 MiB FAT volume
    // VERIFY: fatfs 0.3 API surface — format_volume + create_file calls below
    // are the shape; adjust if the crate version differs.
    fatfs::format_volume(
        &img,
        fatfs::FormatVolumeOptions::new().volume_label(*b"CIDATA     "),
    )?;
    let fs_ = fatfs::FileSystem::new(&img, fatfs::FsOptions::new())?;
    let root = fs_.root_dir();
    root.create_file("meta-data")?.write_all(meta.as_bytes())?;
    root.create_file("user-data")?.write_all(user.as_bytes())?;
    Ok(())
}

// The unified runner: no fw_cfg cmd -> normal login (setup/install);
// cmd present -> mount project ro, run agent, power off (run).
fn user_data(p: &Platform) -> String {
    let console = p.console;
    format!(
        "#cloud-config
packages:
  - nodejs
  - npm
write_files:
  - path: /usr/local/sbin/readonly-runner
    permissions: '0755'
    content: |
      #!/bin/sh
      FW=/sys/firmware/qemu_fw_cfg/by_name/opt/readonly
      INSTALL=\"$(cat \"$FW/install/raw\" 2>/dev/null)\"
      CMD=\"$(cat \"$FW/cmd/raw\" 2>/dev/null)\"
      if [ -n \"$INSTALL\" ]; then
        echo '>> Installing agent into the base image...'
        sh -c \"$INSTALL\"
        echo '>> Done. Log in to your agent now, then type: poweroff'
        exec /bin/sh
      elif [ -n \"$CMD\" ]; then
        mkdir -p /mnt/project
        mount -t 9p -o trans=virtio,version=9p2000.L,ro project /mnt/project
        cd /mnt/project
        $CMD
        poweroff -f
      else
        exec /bin/sh
      fi
runcmd:
  - modprobe 9pnet_virtio qemu_fw_cfg 2>/dev/null || true
  - sed -i 's|^{console}::.*|{console}::respawn:/usr/local/sbin/readonly-runner|' /etc/inittab
  - poweroff
"
    )
}


fn provision(p: &Platform, base: &Path, seed: &Path) -> Result<()> {
    let qemu = which(p.qemu)?;
    let mut c = Command::new(qemu);
    c.args(["-machine", &format!("{},accel={}", p.machine, p.accel)]);
    c.args(["-m", "1024"]); // headroom for apk + npm during first boot
    c.arg("-nographic");
    c.args(["-drive", &format!("file={},if=virtio,format=qcow2", base.display())]);
    c.args(["-drive", &format!("file={},if=virtio,format=raw", seed.display())]);
    c.args(["-netdev", "user,id=n0"]);
    c.args(["-device", "virtio-net-pci,netdev=n0"]);
    // VERIFY: aarch64 'virt' needs UEFI firmware to boot; stage edk2 and add:
    // if p.needs_uefi { c.args(["-bios", "<path-to-edk2-aarch64-code.fd>"]); }
    let status = c.status().context("running provisioning VM")?;
    if !status.success() {
        bail!("provisioning VM exited with {status}");
    }
    Ok(())
}
