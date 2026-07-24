#include "kv.hpp" 

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace readonly::core::kv {
namespace fs = std::filesystem;

std::string_view trim(std::string_view s) {
    constexpr std::string_view ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

Result<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return fail(std::format("cannot read {}", p.string()));
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return fail(std::format("error reading {}", p.string()));
    return ss.str();
}

// POSIX for fsync + rename-into-place. Windows port needs a MoveFileEx shim
Result<void> write_file_atomic(const fs::path& p, std::string_view contents) {
    fs::path tmp = p;
    tmp += ".tmp";

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) 
        return fail(std::format("cannot open {}: {}", tmp.string(), std::strerror(errno)));

    std::size_t off = 0;
    while (off < contents.size()) {
        const ssize_t n = ::write(fd, contents.data() + off, contents.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string why = std::strerror(errno);
            ::close(fd);
            std::error_code ec;
            fs::remove(tmp, ec);
            return fail(std::format("write failed on {}: {}", tmp.string(), why));
        }
        off += static_cast<std::size_t>(n);
    }

    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        const std::string why = std::strerror(errno);
        std::error_code ec;
        fs::remove(tmp, ec);
        return fail(std::format("flush failed on {}: {}", tmp.string(), why));
    }

    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return fail(std::format("cannot commit {}: {}", p.string(), ec.message()));
    }
    return {};
}

Result<std::vector<Entry>> parse_kv(std::string_view text, const fs::path& origin) {
    std::vector<Entry> out;
    std::size_t line_no = 0;

    for (std::size_t pos = 0; pos <= text.size();) {
        const auto nl   = text.find('\n', pos);
        const auto line = text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        ++line_no;

        const auto t = trim(line);
        if (t.empty() || t.front() == '#') continue;

        const auto eq = t.find('=');
        if (eq == std::string_view::npos) 
            return fail(std::format("{}:{}: expected key=value", origin.string(), line_no));

        const auto key = trim(t.substr(0, eq));
        if (key.empty())
            return fail(std::format("{}:{}: empty key", origin.string(), line_no));

        out.emplace_back(std::string(key), std::string(trim(t.substr(eq + 1))));
    }
    return out;
}

std::vector<std::string> split_list(std::string_view s, char delim) {
    std::vector<std::string> out;
    for (std::size_t pos = 0; pos <= s.size();) {
        const auto d    = s.find(delim, pos);
        const auto part = s.substr(pos, (d == std::string_view::npos ? s.size() : d) - pos);
        pos = (d == std::string_view::npos) ? s.size() + 1 : d + 1;

        if (const auto t = trim(part); !t.empty()) out.emplace_back(t);
    }
    return out; 
}

std::string join_list(const std::vector<std::string>& v, char delim) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += delim;
        out += v[i];
    }
    return out;
}

} // namespace readonly::core::kv