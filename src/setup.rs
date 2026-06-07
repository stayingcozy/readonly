use crate::platform::{Arch, Platform};
use anyhow::{bail, Context, Result};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;
use which::which;

// VERIFY: exact Alpine "cloud" qcow2 URLs change every release — fill from
// https://alpinelinux.org/cloud/ (nocloud variant) and pin a sha256 for each.
fn image_url(arch: Arch) -> &'static str {
    match arch {
        Arch::X86_64 => "https://example/alpine-nocloud-x86_64.qcow2",   // TODO
        Arch::Aarch64 => "https://example/alpine-nocloud-aarch64.qcow2", // TODO
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

fn ensure_tools(p: &Platform) -> Result<()> {
    which(p.qemu).with_context(|| format!("{} not found in PATH", p.qemu))?;
    which("qemu-img").context("qemu-img not found in PATH (install QEMU)")?;
    Ok(())
}

fn fetch_image(p: &Platform, cache: &Path) -> Result<PathBuf> {
    let dest = cache.join(format!("alpine-{:?}.qcow2", p.arch).to_lowercase());
    if dest.exists() {
        return Ok(dest);
    }
    let url = image_url(p.arch);
    println!("Downloading base image: {url}");
    let resp = ureq::get(url).call().context("downloading base image")?;
    let mut reader = resp.into_reader();
    let tmp = dest.with_extension("part");
    let mut file = fs::File::create(&tmp)?;
    std::io::copy(&mut reader, &mut file)?;
    // TODO: verify sha256 against the pinned checksum before promoting.
    fs::rename(&tmp, &dest)?;
    Ok(dest)
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
    let console = p.console; // ttyS0 (x86) / ttyAMA0 (aarch64)
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
      CMD=\"$(cat /sys/firmware/qemu_fw_cfg/by_name/opt/readonly/cmd/raw 2>/dev/null)\"
      if [ -z \"$CMD\" ]; then
        exec /sbin/getty -L {console} 115200 vt100   # setup/install: a shell
      fi
      mkdir -p /mnt/project
      mount -t 9p -o trans=virtio,version=9p2000.L,ro project /mnt/project
      cd /mnt/project
      $CMD
      poweroff -f                                    # run: done, tear down
runcmd:
  - modprobe 9pnet_virtio qemu_fw_cfg 2>/dev/null || true
  # VERIFY: inittab format on the Alpine cloud image; this swaps the console
  # getty for our runner so all three modes share one entry.
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
