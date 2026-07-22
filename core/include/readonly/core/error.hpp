#pragma once 
#include <expected>
#include <string>

namespace readonly::core {

struct Error { std::string message; };
template <typename T> using Result = std::expected<T, Error>;
inline std::unexpected<Error> fail(std::string msg) {
    return std::unexpected(Error{std::move(msg)});
}

} // namespace readonly::core