#pragma once 
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "readonly/core/error.hpp"

namespace readonly::core::kv {

    struct Entry { std::string key; std::string value; };

    std::string_view trim(std::string_view s);

    Result<std::string> read_file(const std::filesystem::path& p);
    Result<void>        write_file_atomic(const std::filesystem::path& p, std::string_view contents);

    // Blank lines and '#' comments skipped. Malformed line -> error naming file:line
    Result<std::vector<Entry>> parse_kv(std::string_view text,
                                        const std::filesystem::path& origin);

    std::vector<std::string> split_list(std::string_view s, char delim = ',');
    std::string              join_list(const std::vector<std::string>& v, char delim = ',');

} // namespace readonly::core::kv