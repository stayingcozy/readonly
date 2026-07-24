#include "readonly/core/mounts.hpp"

#include <format>
#include <random>
#include <system_error>
#include <unistd.h>

namespace readonly::core {

// ---- MaskSpec------------------

bool MaskSpec::matches(std::string_view basename) const {
    for (const auto& n : names ) {
        if (n.empty()) continue;
        if (basename == n) return true;
        // dotted family: basename == n + '.' + anything
        if (basename.size() > n.size() &&
            basename.starts_with(n) &&
            basename[n.size()] == '.')
            return true;
    }
    return false;
}

// ---- RunScratch lifecycle---------------
namespace {
std::string gen_run_id() {
    std::random_device rd;
    const std::uint64_t r = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    return std::format("run-{}-{:016x}", ::getpid(), r);
}
} // namespace

Result<RunScratch> RunScratch::create(const Paths& paths) {
    RunScratch s;

    for (int attempt = 0; attempt < 4 && s.run_dir_.empty(); ++attempt) {
        const fs::path dir = paths.scratch_root() / gen_run_id();
        std::error_code ec;
        if (fs::create_directory(dir, ec)) { s.run_dir_ = dir; break; } // created
        if (ec)
            return fail(std::format("cannot create scratch dir {}: {}", dir.string(), ec.message()));
        // else: already existed (unlikely) -> retry
    }
    if (s.run_dir_.empty())
        return fail("could not allocate a unique scratch directory");

    s.src_ = s.run_dir_ / "src";
    s.out_ = s.run_dir_ / "out";

    std::error_code ec;
    fs::create_directory(s.src_, ec);
    if (ec) return fail(std::format("cannot create {}: {}", s.src_.string(), ec.message()));
    fs::create_directory(s.out_, ec);
    if (ec) return fail(std::format("cannot create {}: {}", s.out_.string(), ec.message()));

    return s;
}

void RunScratch::cleanup() noexcept {
    if (!run_dir_.empty()) {
        std::error_code ec;
        fs::remove_all(run_dir_, ec); 
    }
}

RunScratch::~RunScratch() { cleanup(); }

RunScratch::RunScratch(RunScratch&& o) noexcept
    : run_dir_(std::move(o.run_dir_)), src_(std::move(o.src_)), out_(std::move(o.out_)) {
        o.run_dir_.clear(); // move from must not delete
}

RunScratch& RunScratch::operator=(RunScratch&& o) noexcept {
    if (this != &o) {
        cleanup();
        run_dir_ = std::move(o.run_dir_);
        src_     = std::move(o.src_);
        out_     = std::move(o.out_);
        o.run_dir_.clear();
    }
    return *this;
}

fs::path RunScratch::release() {
    fs::path d = run_dir_;
    run_dir_.clear();  // cleanup() no-ops; caller owns dir
    return d;
}

// ---- the mirror -----------

Result<MirrorStats> RunScratch::mirror_source(const fs::path& target, const MaskSpec& mask) {
    std::error_code ec;

    // Resolve the root once. Symlinks *inside* are handled below;
    // resolving the root the user explicitly chose 
    const fs::path root = fs::canonical(target, ec);
    if (ec) 
        return fail(std::format("cannot resolve source path {}: {}", target.string(), ec.message()));
    if (!fs::is_directory(root, ec)) 
        return fail(std::format("source path {} is not a directory", root.string()));

    MirrorStats stats;

    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec)
        return fail(std::format("cannot read {}: {}", root.string(), ec.message()));
    
    for (; it != end; it.increment(ec)) {
        if (ec) 
            return fail(std::format("error walking {}: {}", root.string(), ec.message()));
        
        const fs::path&   p    = it->path();
        const std::string name = p.filename().string();

        std::error_code sec;
        const fs::file_status st = fs::symlink_status(p, sec); 
        if (sec) continue; 

        // Security: never mirror a symlink
        // expose secret, masked file under diff name
        if (fs::is_symlink(st)) {
            ++stats.symlinks_skipped;
            continue;
        }

        if (mask.matches(name)) {
            ++stats.masked; 
            if (fs::is_directory(st)) it.disable_recursion_pending();  // skip whole subtree
            continue;
        }

        const fs::path rel = p.lexically_relative(root); 
        const fs::path dst = src_ / rel;

        if (fs::is_directory(st)) {
            std::error_code dec;
            fs::create_directories(dst, dec);
            if (dec) return fail(std::format("cannot create {}: {}", dst.string(), dec.message()));
            ++stats.dirs;

        } else if (fs::is_regular_file(st)) {
            std::error_code pec;
            fs::create_directories(dst.parent_path(), pec); 
            if (pec) return fail(std::format("cannot create {}: {}",
                                            dst.parent_path().string(), pec.message()));
                
            // 
            std::error_code lec;
            fs::create_hard_link(p, dst, lec);
            if (!lec) {
                ++stats.linked;
            } else {
                // Cross-filesystem (EXDEV) or link-count limit: fall back to a copy
                std::error_code cec;
                fs::copy_file(p, dst, fs::copy_options::overwrite_existing, cec);
                if (cec)
                    return fail(std::format("cannot mirror {} -> {}: {}",
                                            p.string(), dst.string(), cec.message()));
                ++stats.copied;
            }
        } else {
            ++stats.special_skipped;  // device / fifo / socket - never mirrored
        }
    }

    return stats;
}

} // namespace readonly::core