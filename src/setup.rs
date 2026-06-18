use crate::platform::{Arch, Platform};
use std::fs;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
use std::io::{Read, Write};
use sha2::{Digest, Sha512};
use std::path::{Path, PathBuf};
use std::process::Command;
use which::which;

struct ImageSource {
    url: &'static str,
    sha512: &'static str, // empty = not pinned yet, verification skipped
}

// Debian "nocloud" cloud images: ship cloud-init, default to the NoCloud
// datasource our CIDATA seed feeds, and are glibc-native so the official
// curl|bash installers Just Work. To pin sha512, swap `latest` for a dated
// build dir (e.g. .../bookworm/20240xxx-yy/) and paste the matching value
// from its SHA512SUMS — `latest` moves, so a pinned hash there would rot.
fn image_source(arch: Arch) -> ImageSource {
    match arch {
        Arch::X86_64 => ImageSource {
            url: "https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-nocloud-amd64.qcow2",
            sha512: "",
        },
        Arch::Aarch64 => ImageSource {
            url: "https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-nocloud-arm64.qcow2",
            sha512: "",
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
    fs::copy(&pristine, &base).map_err(|e| format!("copying base image: {e}"))?;

    // 2. Build the cloud-init seed in-process (no external ISO/FAT tools).
    let seed = data.join("seed.img");
    write_seed(p, &seed)?;

    // 3. Boot once read-WRITE to provision; cloud-init ends with poweroff.
    println!("Provisioning base VM (installing tools + baking runner)...");
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
        Err(_) => return Err(format!("{name} not found in PATH.\n\nInstall QEMU:\n{}", install_hint()).into()),
    };
    // Confirm it actually runs (catches wrong-arch / broken installs).
    let out = Command::new(&path)
        .arg("--version")
        .output()
        .map_err(|e| format!("failed to execute {}: {e}", path.display()))?;
    if !out.status.success() {
        return Err(format!("{} is present but did not run successfully", path.display()).into());
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
    let dest = cache.join(format!("debian-{:?}.qcow2", p.arch).to_lowercase());
    if dest.exists() {
        return Ok(dest);
    }
    let src = image_source(p.arch);
    println!("Downloading base image: {}", src.url);

    let resp = ureq::get(src.url).call().map_err(|e| format!("downloading base image: {e}"))?;
    let mut reader = resp.into_reader();

    let tmp = dest.with_extension("part");
    let mut file = fs::File::create(&tmp)?;
    let mut hasher = Sha512::new();
    
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
    if src.sha512.is_empty() {
        eprintln!(" warning: no pinned sha512 - skipping verification");
        eprintln!(" (computed {got}; paste it into image_source to enable the check)");
    } else if !got.eq_ignore_ascii_case(src.sha512) {
        let _ = fs::remove_file(&tmp);
        return Err(format!(
            "checksum mismatch for base image\n expected {}\n got   {}",
            src.sha512, got
        ).into());
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

// The unified runner baked into the image, plus the systemd wiring that runs
// it on the serial console at every boot. No fw_cfg -> interactive shell;
// install set -> run it then drop to a shell for login; cmd set -> mount the
// project ro, run the agent, power off (the `run` path).
fn user_data(p: &Platform) -> String {
    let console = p.console;
    format!(
        "#cloud-config
package_update: true
packages:
  - curl
  - ca-certificates
  - git
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
        modprobe 9pnet_virtio 2>/dev/null || true
        mkdir -p /mnt/project
        mount -t 9p -o trans=virtio,version=9p2000.L,ro project /mnt/project
        cd /mnt/project
        $CMD
        poweroff -f
      else
        exec /bin/sh
      fi
  - path: /etc/systemd/system/serial-getty@{console}.service.d/readonly.conf
    permissions: '0644'
    content: |
      [Service]
      ExecStart=
      ExecStart=-/usr/local/sbin/readonly-runner
      Restart=always
  - path: /etc/modules-load.d/readonly.conf
    permissions: '0644'
    content: |
      qemu_fw_cfg
      9pnet_virtio
      9p
runcmd:
  - systemctl daemon-reload
  - systemctl enable serial-getty@{console}.service
  - poweroff
"
    )
}


fn provision(p: &Platform, base: &Path, seed: &Path) -> Result<()> {
    let qemu = which(p.qemu)?;
    let fw = crate::firmware::resolve(p)?; // aarch64 'virt' needs edk2; kept alive below
    let mut c = Command::new(qemu);
    c.args(["-machine", &format!("{},accel={}", p.machine, p.accel)]);
    c.args(["-m", "1024"]); // headroom for apt during first boot
    c.arg("-nographic");
    c.args(&fw.args);
    c.args(["-drive", &format!("file={},if=virtio,format=qcow2", base.display())]);
    c.args(["-drive", &format!("file={},if=virtio,format=raw", seed.display())]);
    c.args(["-netdev", "user,id=n0"]);
    c.args(["-device", "virtio-net-pci,netdev=n0"]);
    let status = c.status().map_err(|e| format!("running provisioning VM: {e}"))?;
    if !status.success() {
        return Err(format!("provisioning VM exited with {status}").into());
    }
    Ok(())
}
