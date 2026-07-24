#include "readonly/core/registry.hpp"
#include "kv.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <format>

namespace readonly::core {

namespace {
constexpr std::array<std::string_view, 7> kReserved{
    "setup", "install", "list", "help", "version", "uninstall", "reauth"};

fs::path meta_path(const Paths& p, std::string_view name) {
    return p.agents_dir() / (std::string{name} + ".meta");
}

std::string now_iso8601() {
    const auto t = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", t);
}
} // namespace

Registry::Registry(Paths paths) : paths_(std::move(paths)) {}

bool Registry::is_reserved(std::string_view name) {
    return std::ranges::find(kReserved, name) != kReserved.end();
}

Result<void> Registry::validate_name(std::string_view name) {
    if (name.empty())     return fail("agent name is empty");
    if (name.size() > 64) return fail("agent name is longer than 64 characters");

    const auto ok_first = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    const auto ok_rest = [&](char c) { return ok_first(c) || c == '.' || c == '_' || c == '-'; };

    if (!ok_first(name.front()))
        return fail(std::format("agent name '{}' must start with a lowercase letter or digit", name));

    for (const char c : name) {
        if (!ok_rest(c)) {
            return fail(std::format(
                "agent name '{}' contains invalid character '{}' (allowed: a-z 0-9 . _ -)", name, c));
        }
    }

    if (is_reserved(name))
        return fail(std::format("'{}' is a reserved command name", name));

    return {};
}

fs::path Registry::overlay_path(std::string_view name) const {
    return paths_.agents_dir() / (std::string{name} + ".qcow2");
}

bool Registry::exists(std::string_view name) const {
    return fs::exists(overlay_path(name)); // the qcow2 is the source of truth
}

Result<AgentDescriptor> Registry::resolve(std::string_view name) const {
    if (auto v = validate_name(name); !v) return std::unexpected(v.error());
    if (!exists(name)) 
        return fail(std::format("agent '{}' is not installed", name));

    AgentDescriptor d;
    d.name    = std::string{name};    // yar the file stem but nay the file contents
    d.run_cmd = d.name;               // name is binary as default
    d.surface = shared::Surface::Cli; 

    // Missing / corrupted .meta is ok: the overlay is the key
    if (const fs::path mp = meta_path(paths_, name); fs::exists(mp)) {
        if (auto text = kv::read_file(mp)) {
            if (auto entries = kv::parse_kv(*text, mp)) {
                for (const auto& [key, value] : *entries) {
                    if (key == "surface")          d.surface = (value == "gui") ? shared::Surface::Gui : shared::Surface::Cli;
                    else if (key == "run" && !value.empty()) d.run_cmd = value;
                    else if (key == "install_cmd") d.install_cmd = value;
                    else if (key == "created")     d.created = value;
                }
            }
        }
    }
    return d;
}

Result<void> Registry::write(const AgentDescriptor& d) const {
    if (auto v = validate_name(d.name); !v) return std::unexpected(v.error());

    // key=value has no escaping, so newline would corrupt file on parse
    for (const auto* field : {&d.run_cmd, &d.install_cmd, &d.created}) {
        if (field->contains('\n') || field->contains('\r'))
            return fail("agent descriptor fields must not contain newlines");
    }

    std::error_code ec;
    fs::create_directories(paths_.agents_dir(), ec);
    if (ec && !fs::is_directory(paths_.agents_dir()))
        return fail("agent descriptor fields must not contain newlines");
    
    const std::string body = 
        std::format("# agent: {}\n"
                    "surface={}\n"
                    "run={}\n"
                    "install_cmd={}\n"
                    "created={}\n",
                    d.name,
                    d.surface == shared::Surface::Gui ? "gui" : "cli",
                    d.run_cmd,
                    d.install_cmd,
                    d.created.empty() ? now_iso8601() : d.created);
    
    return kv::write_file_atomic(meta_path(paths_, d.name), body);
}

Result<std::vector<AgentDescriptor>> Registry::list() const {
    std::vector<AgentDescriptor> out;

    std::error_code ec;
    if (!fs::is_directory(paths_.agents_dir(), ec)) return out;  // nothing installed yet

    for (fs::directory_iterator it(paths_.agents_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".qcow2") continue;

        const std::string name = it->path().stem().string();
        if (!validate_name(name)) continue;    // junk in the trunk but must not break me!

        if (auto d = resolve(name)) out.push_back(std::move(*d));
    }

    std::ranges::sort(out, {}, &AgentDescriptor::name);  // stable output across filesystems
    return out; 
}

} // namespace readonly::core