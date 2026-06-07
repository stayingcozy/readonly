use anyhow::Result;
use globset::{Glob, GlobSet, GlobSetBuilder};
use std::fs;
use std::path::Path;
use walkdir::WalkDir;

/// .env auto-masks: file or folder, at any depth.
fn default_masks() -> Vec<String> {
    vec![
        "**/.env".into(),    // .env file OR .env/ folder (skip-descend handles the dir)
        "**/.env.*".into(),  // .env.local, .env.production, .env.anything
        "**/.envrc".into(),
    ]
}

/// Resolve the full mask list from CLI flags.
pub fn resolve_masks(no_mask: bool, extra: &[String]) -> Vec<String> {
    if no_mask {
        return vec![];
    }
    let mut m = default_masks();
    for e in extra {
        let clean = e.trim_start_matches("./");
        m.push(format!("**/{clean}"));
        m.push(clean.to_string());
    }
    m
}

pub fn build_globset(patterns: &[String]) -> Result<GlobSet> {
    let mut b = GlobSetBuilder::new();
    for p in patterns {
        b.add(Glob::new(p)?);
    }
    Ok(b.build()?)
}

/// Copy `src` into `dest`, skipping anything matching `masks`.
/// Symlinks are skipped for MVP (a symlink could point outside the share).
pub fn build_filtered_share(src: &Path, masks: &GlobSet, dest: &Path) -> Result<()> {
    let walker = WalkDir::new(src).into_iter().filter_entry(|e| {
        let rel = e.path().strip_prefix(src).unwrap_or(e.path());
        !masks.is_match(rel) // false -> prune (skips dir contents too)
    });

    for entry in walker {
        let entry = entry?;
        let rel = entry.path().strip_prefix(src)?;
        if rel.as_os_str().is_empty() {
            continue;
        }
        let target = dest.join(rel);
        let ft = entry.file_type();
        if ft.is_dir() {
            fs::create_dir_all(&target)?;
        } else if ft.is_file() {
            if let Some(p) = target.parent() {
                fs::create_dir_all(p)?;
            }
            fs::copy(entry.path(), &target)?;
        }
        // symlinks intentionally skipped
    }
    Ok(())
}
